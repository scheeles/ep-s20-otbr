/* Best-effort mirror of the ESP-IDF log stream to a remote UDP syslog server.
 *
 * Design constraints (the vprintf hook sits on the logging hot path and may be
 * entered by any task, on either core, at any priority):
 *   - never block            -> non-blocking socket + MSG_DONTWAIT, drop on failure
 *   - never allocate         -> bounded stack buffers only
 *   - never recurse          -> per-task guard, and no ESP_LOGx from the hook
 *   - never break the console-> the previous handler is always called first
 *
 * Note that ESP_EARLY_LOGx / ESP_DRAM_LOGx (ISR and pre-heap logging) bypass
 * esp_log_set_vprintf() entirely, so this hook is never entered from an ISR.
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
#include "freertos/task.h"
#include "lwip/inet.h"
#include "lwip/sockets.h"

#include "net_syslog.h"

#define TAG "net_syslog"

/* Bounded stack buffers. The hook runs on the caller's stack, so a logging task
 * needs roughly 1.1 kB of headroom; shrink these two if a task with a tight
 * stack has to log. */
#define SYSLOG_MSG_MAX 480
#define SYSLOG_PKT_MAX 560
#define SYSLOG_HOSTNAME_MAX 64
#define SYSLOG_TIMESTAMP_MAX 20

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

/* Destination double buffer. net_syslog_set_server() fills the spare slot and
 * then publishes it with a single write to s_dest_idx, so a concurrent logger
 * either sees the whole old address or the whole new one — never a half-written
 * one — without taking a lock on the logging hot path. */
static struct sockaddr_in s_dest[2];
static volatile uint8_t s_dest_idx = 0;
static volatile bool s_dest_valid = false;

/* Recursion guard. Recursion is by definition same-task, so remembering the
 * task currently inside the send path is enough and needs no lock: concurrent
 * loggers from other tasks are unaffected (a lost write can at worst let one
 * line through the guard or drop one line, never corrupt state). */
static volatile TaskHandle_t s_emitting_task = NULL;

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
 * @brief Format one log line as RFC3164 and push it out, best effort.
 *
 * The device clock is usually wrong; the receiver stamps its own arrival time,
 * so a plausible timestamp is good enough.
 */
static void net_syslog_emit(int sock, const char *fmt, va_list args)
{
    char msg[SYSLOG_MSG_MAX];
    char pkt[SYSLOG_PKT_MAX];
    char timestamp[SYSLOG_TIMESTAMP_MAX];
    const struct sockaddr_in *dest;
    struct tm utc;
    time_t now;
    size_t len;
    int written;

    /* Read the published slot index exactly once. */
    dest = &s_dest[s_dest_idx & 1];

    written = vsnprintf(msg, sizeof(msg), fmt, args);
    if (written <= 0) {
        return;
    }
    /* vsnprintf returns the length it *would* have written: clamp to what fits. */
    len = (written < (int)sizeof(msg)) ? (size_t)written : sizeof(msg) - 1;

    len = strip_ansi_escapes(msg, len);
    while (len > 0 && (msg[len - 1] == '\n' || msg[len - 1] == '\r')) {
        msg[--len] = '\0';
    }
    if (len == 0) {
        return;
    }

    now = time(NULL);
    if (gmtime_r(&now, &utc) == NULL || strftime(timestamp, sizeof(timestamp), "%b %e %H:%M:%S", &utc) == 0) {
        strcpy(timestamp, "Jan  1 00:00:00");
    }

    written = snprintf(pkt, sizeof(pkt), "<%d>%s %s " SYSLOG_APP_TAG ": %s",
                       SYSLOG_FACILITY_BASE + severity_from_line(msg), timestamp, s_hostname, msg);
    if (written <= 0) {
        return;
    }
    len = (written < (int)sizeof(pkt)) ? (size_t)written : sizeof(pkt) - 1;

    (void)sendto(sock, pkt, len, MSG_DONTWAIT, (const struct sockaddr *)dest, sizeof(*dest));
}

/**
 * @brief Chained esp_log vprintf hook.
 */
static int net_syslog_vprintf(const char *fmt, va_list args)
{
    vprintf_like_t next = s_next_vprintf;
    TaskHandle_t self;
    va_list chain_args;
    va_list emit_args;
    int sock;
    int ret;

    /* Console first and unconditionally, so remote logging stays purely additive.
     * `next` is only NULL in the tiny window inside net_syslog_start() between
     * installing this hook and storing the previous one; falling back to
     * vprintf() there keeps the line on the console. */
    va_copy(chain_args, args);
    ret = next ? next(fmt, chain_args) : vprintf(fmt, chain_args);
    va_end(chain_args);

    sock = s_sock;
    if (sock < 0 || !s_dest_valid) {
        return ret;
    }

    self = xTaskGetCurrentTaskHandle();
    if (s_emitting_task == self) {
        /* Something under sendto() logged: drop rather than recurse. */
        return ret;
    }
    s_emitting_task = self;

    va_copy(emit_args, args);
    net_syslog_emit(sock, fmt, emit_args);
    va_end(emit_args);

    s_emitting_task = NULL;
    return ret;
}

esp_err_t net_syslog_set_server(const char *server_ip, uint16_t port)
{
    struct sockaddr_in dest;
    uint8_t spare;

    if (server_ip == NULL || server_ip[0] == '\0' || port == 0) {
        /* Stop sending, but keep the socket and the hook in place so a server
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

    /* Fill the slot the log hook is not reading, then publish it. */
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

    /* Non-blocking so a full lwip tx queue can never stall a logging task. */
    flags = fcntl(sock, F_GETFL, 0);
    if (flags < 0 || fcntl(sock, F_SETFL, flags | O_NONBLOCK) < 0) {
        ESP_LOGW(TAG, "Failed to set syslog socket non-blocking (errno %d), remote logging disabled", errno);
        close(sock);
        return;
    }

    /* Set the destination before the hook can observe the socket. A bad or
     * empty address simply leaves the module dormant until the web UI
     * configures one. */
    net_syslog_set_server(server_ip, port);

    s_sock = sock;
    s_next_vprintf = esp_log_set_vprintf(net_syslog_vprintf);
    s_started = true;

    if (!s_dest_valid) {
        ESP_LOGI(TAG, "Remote syslog ready, waiting for a server to be configured");
    }
}
