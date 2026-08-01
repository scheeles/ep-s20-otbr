/* Best-effort mirror of the ESP-IDF log stream to a remote UDP syslog server.
 *
 * The log hook may be entered by ANY task, on either core, including lwip's own
 * tcpip thread (esp_netif logs from its netif/ND6/DHCP callbacks). That rules
 * out touching a socket from the hook: with LWIP_TCPIP_CORE_LOCKING disabled —
 * the ESP-IDF default — every socket call posts a message to the tcpip mailbox
 * and waits on a semaphore for the tcpip thread to run it. Called *from* the
 * tcpip thread that is a permanent self-deadlock, and called from any other
 * task it still stalls that task for a full round trip. Neither is acceptable
 * on the logging path, and a non-blocking socket does not help: O_NONBLOCK and
 * MSG_DONTWAIT only govern whether lwip waits for buffer space once the tcpip
 * thread picks the message up.
 *
 * So the hook only formats the line and drops it into a fixed-size queue, and a
 * dedicated worker task owns the socket and does the sending. Consequences:
 *   - no lwip call ever runs on a caller's task, so no deadlock and no stall
 *   - the packet buffers live on the worker's stack, so a logging task lends
 *     only ~300 B instead of ~1.1 kB
 *   - the queue and worker are created once, so the hook still never allocates
 *   - a full queue drops lines rather than waiting for anything
 *
 * ESP_EARLY_LOGx / ESP_DRAM_LOGx (ISR and pre-heap logging) bypass
 * esp_log_set_vprintf() entirely, so the hook is not normally reachable from an
 * ISR; it checks anyway, because queue APIs are not ISR-safe.
 */

#include <errno.h>
#include <fcntl.h>
#include <stdbool.h>
#include <stdio.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/task.h"
#include "lwip/inet.h"
#include "lwip/sockets.h"

#include "net_syslog.h"

#define TAG "net_syslog"

/* One queued line. Longer lines are truncated; ESP-IDF log lines are far
 * shorter in practice. Sized to keep the queue's static cost modest. */
#define SYSLOG_LINE_MAX 256
/* Assembled datagram. Lives on the worker's stack only. */
#define SYSLOG_PKT_MAX 560
#define SYSLOG_HOSTNAME_MAX 64
#define SYSLOG_TIMESTAMP_MAX 20

/* Queue depth trades RAM for burst tolerance: 16 * 256 B = 4 kB, allocated
 * once when a server is first configured. */
#define SYSLOG_QUEUE_DEPTH 16
#define SYSLOG_TASK_STACK_SIZE 3584
/* Below the web/OTA tasks: shipping logs must never outrank real work. */
#define SYSLOG_TASK_PRIORITY 3

/* RFC3164 tag, i.e. the "otbr:" in "<134>Jan  1 00:00:00 host otbr: message". */
#define SYSLOG_APP_TAG "otbr"

/* RFC3164 PRI = facility * 8 + severity. Facility local0 (16) -> base 128. */
#define SYSLOG_FACILITY_BASE (16 * 8)
#define SYSLOG_SEVERITY_ERR 3
#define SYSLOG_SEVERITY_WARNING 4
#define SYSLOG_SEVERITY_INFO 6
#define SYSLOG_SEVERITY_DEBUG 7

static volatile int s_sock = -1;
static char s_hostname[SYSLOG_HOSTNAME_MAX] = "esp32";
static vprintf_like_t s_next_vprintf = NULL;
static bool s_started = false;

/* Published after the worker task exists, so the hook never queues into a
 * queue nobody is draining. */
static QueueHandle_t volatile s_queue = NULL;

/* Destination double buffer. net_syslog_set_server() fills the spare slot and
 * then publishes it with a single write to s_dest_idx, so the worker either
 * sees the whole old address or the whole new one — never a half-written one —
 * without taking a lock. */
static struct sockaddr_in s_dest[2];
static volatile uint8_t s_dest_idx = 0;
static volatile bool s_dest_valid = false;

/* Set while the worker is inside sendto(). If anything under lwip logs, that
 * line is dropped instead of being queued back into the worker it came from. */
static volatile TaskHandle_t s_sending_task = NULL;

/**
 * @brief Strip ANSI CSI escape sequences (ESP-IDF colour codes) in place.
 *
 * @param[in,out] str  NUL-terminated buffer to filter.
 * @param[in]     len  Length of @p str, excluding the terminator.
 * @return Length of the filtered string.
 */
static size_t strip_ansi_escapes(char *str, size_t len)
{
    size_t read = 0;
    size_t write = 0;

    while (read < len) {
        if (str[read] != '\033') {
            str[write++] = str[read++];
            continue;
        }
        read++;
        if (read < len && str[read] == '[') {
            read++;
            /* CSI runs until a final byte in the range 0x40-0x7E; ESP-IDF only
             * ever emits the 'm' (SGR) form, so scanning for a letter is enough. */
            while (read < len && !((str[read] >= 'A' && str[read] <= 'Z') || (str[read] >= 'a' && str[read] <= 'z'))) {
                read++;
            }
            if (read < len) {
                read++;
            }
        }
    }
    str[write] = '\0';
    return write;
}

/**
 * @brief Map the ESP-IDF level letter at the start of a formatted line to a
 *        syslog severity.
 *
 * ESP-IDF lines look like "I (12345) TAG: message", so the level is the first
 * character followed by a space. Anything else (raw printf output, continuation
 * lines) falls back to info.
 */
static int severity_from_line(const char *msg)
{
    if (msg[0] == '\0' || msg[1] != ' ') {
        return SYSLOG_SEVERITY_INFO;
    }
    switch (msg[0]) {
    case 'E':
        return SYSLOG_SEVERITY_ERR;
    case 'W':
        return SYSLOG_SEVERITY_WARNING;
    case 'I':
        return SYSLOG_SEVERITY_INFO;
    case 'D':
    case 'V':
        return SYSLOG_SEVERITY_DEBUG;
    default:
        return SYSLOG_SEVERITY_INFO;
    }
}

/**
 * @brief Wrap one queued line in an RFC3164 frame and send it.
 *
 * Runs on the worker task, never on a caller's task. The device clock is
 * usually wrong; the receiver stamps its own arrival time, so a plausible
 * timestamp is good enough.
 */
static void net_syslog_send_line(int sock, const char *line)
{
    char pkt[SYSLOG_PKT_MAX];
    char timestamp[SYSLOG_TIMESTAMP_MAX];
    const struct sockaddr_in *dest;
    struct tm utc;
    time_t now;
    size_t len;
    int written;

    /* Read the published slot index exactly once. */
    dest = &s_dest[s_dest_idx & 1];

    now = time(NULL);
    if (gmtime_r(&now, &utc) == NULL || strftime(timestamp, sizeof(timestamp), "%b %e %H:%M:%S", &utc) == 0) {
        strcpy(timestamp, "Jan  1 00:00:00");
    }

    written = snprintf(pkt, sizeof(pkt), "<%d>%s %s " SYSLOG_APP_TAG ": %s",
                       SYSLOG_FACILITY_BASE + severity_from_line(line), timestamp, s_hostname, line);
    if (written <= 0) {
        return;
    }
    len = (written < (int)sizeof(pkt)) ? (size_t)written : sizeof(pkt) - 1;

    /* Guard the window in which lwip could log back at us. */
    s_sending_task = xTaskGetCurrentTaskHandle();
    (void)sendto(sock, pkt, len, MSG_DONTWAIT, (const struct sockaddr *)dest, sizeof(*dest));
    s_sending_task = NULL;
}

/**
 * @brief Worker task: owns every socket call this component makes.
 */
static void net_syslog_worker(void *param)
{
    QueueHandle_t queue = (QueueHandle_t)param;
    char line[SYSLOG_LINE_MAX];

    for (;;) {
        if (xQueueReceive(queue, line, portMAX_DELAY) != pdTRUE) {
            continue;
        }
        int sock = s_sock;
        if (sock < 0 || !s_dest_valid) {
            continue; /* Configured away while queued: drop it. */
        }
        line[SYSLOG_LINE_MAX - 1] = '\0';
        net_syslog_send_line(sock, line);
    }
}

/**
 * @brief Create the queue and worker task once, on first use.
 */
static bool net_syslog_worker_ensure(void)
{
    QueueHandle_t queue;

    if (s_queue != NULL) {
        return true;
    }

    queue = xQueueCreate(SYSLOG_QUEUE_DEPTH, SYSLOG_LINE_MAX);
    if (queue == NULL) {
        ESP_LOGW(TAG, "Failed to allocate the syslog queue, remote logging disabled");
        return false;
    }

    /* Hand the queue to the task directly: s_queue is only published once the
     * consumer is guaranteed to exist. */
    if (xTaskCreate(net_syslog_worker, "syslog_tx", SYSLOG_TASK_STACK_SIZE, queue, SYSLOG_TASK_PRIORITY, NULL) !=
        pdPASS) {
        vQueueDelete(queue);
        ESP_LOGW(TAG, "Failed to start the syslog worker task, remote logging disabled");
        return false;
    }

    s_queue = queue;
    return true;
}

/**
 * @brief Chained esp_log vprintf hook. Formats and queues; never touches lwip.
 */
static int net_syslog_vprintf(const char *fmt, va_list args)
{
    vprintf_like_t next = s_next_vprintf;
    QueueHandle_t queue;
    char line[SYSLOG_LINE_MAX];
    va_list chain_args;
    va_list line_args;
    size_t len;
    int written;
    int ret;

    /* Console first and unconditionally, so remote logging stays purely additive.
     * `next` is only NULL in the tiny window inside net_syslog_start() between
     * installing this hook and storing the previous one; falling back to
     * vprintf() there keeps the line on the console. */
    va_copy(chain_args, args);
    ret = next ? next(fmt, chain_args) : vprintf(fmt, chain_args);
    va_end(chain_args);

    queue = s_queue;
    if (queue == NULL || !s_dest_valid) {
        return ret;
    }
    if (xPortInIsrContext()) {
        return ret; /* Queue APIs are not ISR-safe, and this is best effort. */
    }
    if (s_sending_task == xTaskGetCurrentTaskHandle()) {
        return ret; /* Something under sendto() logged: drop rather than loop. */
    }

    va_copy(line_args, args);
    written = vsnprintf(line, sizeof(line), fmt, line_args);
    va_end(line_args);

    if (written <= 0) {
        return ret;
    }
    /* vsnprintf returns the length it *would* have written: clamp to what fits. */
    len = (written < (int)sizeof(line)) ? (size_t)written : sizeof(line) - 1;

    len = strip_ansi_escapes(line, len);
    while (len > 0 && (line[len - 1] == '\n' || line[len - 1] == '\r')) {
        line[--len] = '\0';
    }
    if (len == 0) {
        return ret;
    }

    /* Zero timeout: a full queue drops the line instead of delaying the caller. */
    (void)xQueueSend(queue, line, 0);
    return ret;
}

esp_err_t net_syslog_set_server(const char *server_ip, uint16_t port)
{
    struct sockaddr_in dest;
    uint8_t spare;

    if (server_ip == NULL || server_ip[0] == '\0' || port == 0) {
        /* Stop sending, but keep the socket and the worker in place so a server
         * can be configured again later without a reboot. */
        s_dest_valid = false;
        ESP_LOGI(TAG, "Remote syslog disabled");
        return ESP_OK;
    }

    memset(&dest, 0, sizeof(dest));
    dest.sin_family = AF_INET;
    dest.sin_port = htons(port);
    if (inet_pton(AF_INET, server_ip, &dest.sin_addr) != 1) {
        ESP_LOGW(TAG, "Invalid syslog server address '%s', keeping previous setting", server_ip);
        return ESP_ERR_INVALID_ARG;
    }

    if (!net_syslog_worker_ensure()) {
        return ESP_ERR_NO_MEM;
    }

    /* Fill the slot the worker is not reading, then publish it. */
    spare = (uint8_t)((s_dest_idx & 1) ^ 1);
    s_dest[spare] = dest;
    s_dest_idx = spare;
    s_dest_valid = true;

    ESP_LOGI(TAG, "Mirroring logs to syslog://%s:%u as '%s'", server_ip, (unsigned)port, s_hostname);
    return ESP_OK;
}

void net_syslog_start(const char *server_ip, uint16_t port, const char *hostname)
{
    int flags;
    int sock;

    if (s_started) {
        /* Already running: treat a repeated call as a destination update so a
         * DHCP renewal cannot undo a server configured from the web UI. */
        if (server_ip != NULL && server_ip[0] != '\0' && port != 0) {
            net_syslog_set_server(server_ip, port);
        }
        return;
    }

    if (hostname != NULL && hostname[0] != '\0') {
        snprintf(s_hostname, sizeof(s_hostname), "%s", hostname);
        /* The RFC3164 HOSTNAME field is space delimited. */
        for (char *cursor = s_hostname; *cursor != '\0'; ++cursor) {
            if (*cursor == ' ') {
                *cursor = '_';
            }
        }
    }

    sock = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    if (sock < 0) {
        ESP_LOGW(TAG, "Failed to create syslog socket (errno %d), remote logging disabled", errno);
        return;
    }

    /* Non-blocking so the worker cannot be parked by a full lwip tx queue.
     * This is a backstop, not the reason the hot path is safe. */
    flags = fcntl(sock, F_GETFL, 0);
    if (flags < 0 || fcntl(sock, F_SETFL, flags | O_NONBLOCK) < 0) {
        ESP_LOGW(TAG, "Failed to set syslog socket non-blocking (errno %d), remote logging disabled", errno);
        close(sock);
        return;
    }

    /* Set the destination before the hook can observe the socket. A bad or
     * empty address simply leaves the component dormant — no queue, no worker —
     * until the web UI configures one. */
    net_syslog_set_server(server_ip, port);

    s_sock = sock;
    s_next_vprintf = esp_log_set_vprintf(net_syslog_vprintf);
    s_started = true;

    if (!s_dest_valid) {
        ESP_LOGI(TAG, "Remote syslog ready, waiting for a server to be configured");
    }
}
