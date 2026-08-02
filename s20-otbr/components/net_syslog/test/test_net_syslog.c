/* Host tests for the net_syslog component.
 *
 * net_syslog.c is #included rather than linked so the tests can inspect its
 * internal state (whether a worker was created, which destination slot is
 * live). sendto() is redirected to a fake via -Dsendto, which records both the
 * packet and — importantly — the thread it was called on.
 *
 * The headline test is `sendto never runs on a logging task`. Calling a socket
 * from the log hook is what deadlocked lwip's tcpip thread and made a device
 * unreachable; compiling cleanly says nothing about it, so it is asserted here.
 *
 * Tests share one process and run in order: the component is a singleton with
 * static state, and the sequence mirrors a device's life (dormant -> configured
 * -> re-pointed -> switched off).
 */

#include <arpa/inet.h>
#include <pthread.h>
#include <stdarg.h>
#include <stdbool.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>

#include "net_syslog.c"

/* ---------------------------------------------------------------- harness -- */

static int g_failures;
static int g_checks;

#define CHECK(cond, what)                                                                                              \
    do {                                                                                                               \
        g_checks++;                                                                                                    \
        if (cond) {                                                                                                    \
            printf("  ok   %s\n", (what));                                                                             \
        } else {                                                                                                       \
            printf("  FAIL %s   (%s:%d)\n", (what), __FILE__, __LINE__);                                               \
            g_failures++;                                                                                              \
        }                                                                                                              \
    } while (0)

/* ------------------------------------------------------------------ fakes -- */

extern unsigned rtos_stub_dropped;

static vprintf_like_t g_console;
static int g_console_calls;
static char g_console_last[600];

vprintf_like_t esp_log_set_vprintf(vprintf_like_t func)
{
    vprintf_like_t previous = g_console ? g_console : (vprintf_like_t)vprintf;

    g_console = func;
    return previous;
}

/* Stands in for the previously installed handler (UART / web log buffer). */
static int fake_console(const char *fmt, va_list args)
{
    int written = vsnprintf(g_console_last, sizeof(g_console_last), fmt, args);

    g_console_calls++;
    return written;
}

/* Set while the concurrency test runs; see stress_note_destination(). */
static volatile bool g_stress_active;
static void stress_note_destination(const char *ip, int port);

static pthread_mutex_t g_sent_lock = PTHREAD_MUTEX_INITIALIZER;
static pthread_t g_main_thread;
static pthread_t g_last_send_thread;
static int g_sends;
static int g_sends_on_logging_task;
static char g_last_packet[600];
static char g_last_ip[INET_ADDRSTRLEN];
static int g_last_port;

ssize_t fake_sendto(int sock, const void *buf, size_t len, int flags, const struct sockaddr *dest, socklen_t dest_len)
{
    const struct sockaddr_in *in = (const struct sockaddr_in *)dest;

    (void)sock;
    (void)flags;
    (void)dest_len;

    pthread_mutex_lock(&g_sent_lock);
    g_last_send_thread = pthread_self();
    if (pthread_equal(g_last_send_thread, g_main_thread)) {
        g_sends_on_logging_task++;
    }
    inet_ntop(AF_INET, &in->sin_addr, g_last_ip, sizeof(g_last_ip));
    g_last_port = ntohs(in->sin_port);
    memcpy(g_last_packet, buf, len < sizeof(g_last_packet) ? len : sizeof(g_last_packet) - 1);
    g_last_packet[len < sizeof(g_last_packet) ? len : sizeof(g_last_packet) - 1] = '\0';
    g_sends++;
    if (g_stress_active) {
        stress_note_destination(g_last_ip, g_last_port);
    }
    pthread_mutex_unlock(&g_sent_lock);
    return (ssize_t)len;
}

/* ---------------------------------------------------------------- helpers -- */

static void log_line(const char *fmt, ...)
{
    va_list args;

    va_start(args, fmt);
    net_syslog_vprintf(fmt, args);
    va_end(args);
}

/* The worker sends asynchronously; give it a moment to drain. */
static void drain(void)
{
    usleep(150000);
}

static int sends(void)
{
    int count;

    pthread_mutex_lock(&g_sent_lock);
    count = g_sends;
    pthread_mutex_unlock(&g_sent_lock);
    return count;
}

/* ------------------------------------------------------------------ tests -- */

static void test_starts_dormant(void)
{
    printf("start with no server configured\n");
    net_syslog_start("", 514, "ep-s20-otbr");

    CHECK(s_started && s_sock >= 0, "socket and log hook are installed");
    CHECK(s_queue == NULL, "no queue or worker task allocated while dormant");

    g_console_calls = 0;
    log_line("I (1) test: dormant\n");
    drain();
    CHECK(sends() == 0, "nothing is sent without a server");
    CHECK(g_console_calls == 1, "console output still happens");
}

static void test_configure_at_runtime(void)
{
    printf("configuring a server at runtime\n");
    CHECK(net_syslog_set_server("172.17.0.99", 514) == ESP_OK, "a valid endpoint is accepted");
    CHECK(s_queue != NULL, "queue and worker are created on first use");

    log_line("I (2) test: hello\n");
    drain();
    CHECK(sends() == 1, "sending starts without a reboot");
    CHECK(strcmp(g_last_ip, "172.17.0.99") == 0 && g_last_port == 514, "packet goes to the configured endpoint");
}

/* The regression test for the hang: a socket call from the log hook deadlocks
 * lwip when the line originates from its own tcpip thread. */
static void test_sendto_never_on_a_logging_task(void)
{
    printf("sendto stays off the logging path\n");
    CHECK(g_sends_on_logging_task == 0, "sendto never ran on a task that logged");
    CHECK(!pthread_equal(g_last_send_thread, g_main_thread), "sendto ran on the worker task");
}

static void test_packet_format(void)
{
    printf("RFC3164 framing\n");
    log_line("\033[0;33mW (3) test: colour\033[0m\n");
    drain();
    CHECK(strncmp(g_last_packet, "<132>", 5) == 0, "warning maps to PRI 132 (local0.warning)");
    CHECK(strchr(g_last_packet, '\033') == NULL, "ANSI colour escapes are stripped");
    CHECK(strstr(g_last_packet, " ep-s20-otbr otbr: W (3) test: colour") != NULL, "hostname, tag and message layout");
    CHECK(g_last_packet[strlen(g_last_packet) - 1] != '\n', "trailing newline is stripped");

    log_line("E (4) test: boom\n");
    drain();
    CHECK(strncmp(g_last_packet, "<131>", 5) == 0, "error maps to PRI 131");

    log_line("I (5) test: fyi\n");
    drain();
    CHECK(strncmp(g_last_packet, "<134>", 5) == 0, "info maps to PRI 134");

    log_line("D (6) test: detail\n");
    drain();
    CHECK(strncmp(g_last_packet, "<135>", 5) == 0, "debug maps to PRI 135");

    log_line("V (7) test: verbose\n");
    drain();
    CHECK(strncmp(g_last_packet, "<135>", 5) == 0, "verbose maps to PRI 135");

    log_line("plain line with no level\n");
    drain();
    CHECK(strncmp(g_last_packet, "<134>", 5) == 0, "an unparseable level falls back to info");

    /* "Mmm dd hh:mm:ss " is a fixed 16 columns after the 5-column PRI. */
    CHECK(g_last_packet[8] == ' ' && g_last_packet[11] == ' ' && g_last_packet[20] == ' ',
          "timestamp occupies its fixed RFC3164 columns");
}

static void test_console_is_untouched(void)
{
    printf("console output is unchanged\n");
    g_console_calls = 0;
    log_line("I (8) test: %s %d\n", "value", 42);
    CHECK(g_console_calls == 1, "the previous handler is called exactly once");
    CHECK(strcmp(g_console_last, "I (8) test: value 42\n") == 0, "the console receives the original line verbatim");
}

static void test_skips_empty_and_oversized(void)
{
    printf("edge cases\n");
    drain(); /* let anything the previous test logged land first */
    int before = sends();

    log_line("\n");
    log_line("\033[0m\r\n");
    drain();
    CHECK(sends() == before, "empty and escape-only lines are dropped");

    char big[4000];

    memset(big, 'x', sizeof(big) - 1);
    big[sizeof(big) - 1] = '\0';
    log_line("I (9) test: %s\n", big);
    drain();
    CHECK(strlen(g_last_packet) < SYSLOG_PKT_MAX, "an oversized line is truncated within the packet bound");
    CHECK(strncmp(g_last_packet, "<134>", 5) == 0, "a truncated line still carries a valid PRI");
}

static void test_flood_drops_instead_of_blocking(void)
{
    printf("a burst drops lines rather than delaying the caller\n");
    rtos_stub_dropped = 0;
    for (int i = 0; i < 4000; i++) {
        log_line("I (10) test: flood %d\n", i);
    }
    CHECK(rtos_stub_dropped > 0, "a full queue refuses lines");
    drain();
    CHECK(g_sends_on_logging_task == 0, "still no sendto on a logging task under load");
}

static void test_repoint_and_disable(void)
{
    printf("re-pointing and switching off\n");
    CHECK(net_syslog_set_server("10.0.0.5", 1514) == ESP_OK, "a new endpoint is accepted");
    log_line("I (11) test: moved\n");
    drain();
    CHECK(strcmp(g_last_ip, "10.0.0.5") == 0 && g_last_port == 1514, "packets follow the new endpoint");

    CHECK(net_syslog_set_server("not-an-ip", 514) == ESP_ERR_INVALID_ARG, "a malformed address is rejected");
    log_line("I (12) test: unchanged\n");
    drain();
    CHECK(strcmp(g_last_ip, "10.0.0.5") == 0 && g_last_port == 1514, "a rejected address leaves the live one intact");

    CHECK(net_syslog_set_server("", 514) == ESP_OK, "an empty server switches remote logging off");
    int before = sends();

    g_console_calls = 0;
    log_line("I (13) test: quiet\n");
    drain();
    CHECK(sends() == before, "nothing is sent while switched off");
    CHECK(g_console_calls == 1, "console output survives switching syslog off");
    CHECK(s_sock >= 0, "the socket is kept for a later re-enable");

    net_syslog_set_server("10.0.0.5", 514);
    log_line("I (14) test: back\n");
    drain();
    CHECK(sends() == before + 1, "remote logging can be switched back on");
}

static void test_restart_does_not_clobber_runtime_config(void)
{
    printf("a repeated start (DHCP renewal) keeps the configured server\n");
    net_syslog_set_server("10.0.0.5", 1514);
    net_syslog_start("", 514, "some-other-host");
    log_line("I (15) test: renewed\n");
    drain();
    CHECK(strcmp(g_last_ip, "10.0.0.5") == 0 && g_last_port == 1514, "an empty re-start leaves the endpoint alone");
    CHECK(strcmp(s_hostname, "ep-s20-otbr") == 0, "the hostname is not re-read on re-entry");
}

/* Several tasks logging while another re-points the server: no packet may ever
 * be addressed with a half-updated destination. */
#define STRESS_IP_A "10.0.0.5"
#define STRESS_PORT_A 514
#define STRESS_IP_B "172.17.0.99"
#define STRESS_PORT_B 1514
#define STRESS_LOGGERS 4

static volatile bool g_stress_stop;
static int g_stress_torn;
static pthread_t g_stress_loggers[STRESS_LOGGERS];

static void *stress_logger(void *arg)
{
    long id = (long)arg;

    while (!g_stress_stop) {
        log_line("I (16) test: task %ld line\n", id);
    }
    return NULL;
}

static void *stress_repointer(void *arg)
{
    (void)arg;
    while (!g_stress_stop) {
        net_syslog_set_server(STRESS_IP_A, STRESS_PORT_A);
        net_syslog_set_server(STRESS_IP_B, STRESS_PORT_B);
    }
    return NULL;
}

static void test_concurrent_repoint_is_atomic(void)
{
    pthread_t repointer;

    printf("concurrent logging while the server is re-pointed\n");
    net_syslog_set_server(STRESS_IP_A, STRESS_PORT_A);
    g_stress_stop = false;
    g_stress_torn = 0;
    g_stress_active = true;

    for (long i = 0; i < STRESS_LOGGERS; i++) {
        pthread_create(&g_stress_loggers[i], NULL, stress_logger, (void *)i);
    }
    pthread_create(&repointer, NULL, stress_repointer, NULL);
    sleep(1);
    g_stress_stop = true;
    for (int i = 0; i < STRESS_LOGGERS; i++) {
        pthread_join(g_stress_loggers[i], NULL);
    }
    pthread_join(repointer, NULL);
    drain();
    g_stress_active = false;

    CHECK(g_stress_torn == 0, "no packet was addressed with a half-updated destination");
    CHECK(g_sends_on_logging_task == 0, "no sendto on a logging task throughout the stress run");
}

/* Called from the fake sendto during the stress run. */
static void stress_note_destination(const char *ip, int port)
{
    bool is_a = strcmp(ip, STRESS_IP_A) == 0 && port == STRESS_PORT_A;
    bool is_b = strcmp(ip, STRESS_IP_B) == 0 && port == STRESS_PORT_B;

    if (!is_a && !is_b) {
        g_stress_torn++;
    }
}

int main(void)
{
    g_main_thread = pthread_self();
    g_console = fake_console;

    test_starts_dormant();
    test_configure_at_runtime();
    test_sendto_never_on_a_logging_task();
    test_packet_format();
    test_console_is_untouched();
    test_skips_empty_and_oversized();
    test_flood_drops_instead_of_blocking();
    test_repoint_and_disable();
    test_restart_does_not_clobber_runtime_config();
    test_concurrent_repoint_is_atomic();

    printf("\n%d checks, %d failed\n", g_checks, g_failures);
    return g_failures != 0;
}
