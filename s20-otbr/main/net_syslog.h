#pragma once

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief Start mirroring the ESP-IDF log stream to a remote UDP syslog server.
 *
 * Installs a chained `esp_log_set_vprintf()` hook: every log line is still
 * handed to the previously installed handler first (so UART/console output and
 * the web log buffer keep working exactly as before), then formatted as an
 * RFC3164 packet and pushed out with a best-effort, non-blocking `sendto()` on
 * a cached UDP socket. Send failures are silently dropped — logging never
 * blocks and never fails the caller.
 *
 * The syslog PRI is derived from the ESP-IDF level letter of each line
 * (facility `local0`): `E` -> err, `W` -> warning, `I` -> info, `D`/`V` ->
 * debug, defaulting to info when the level cannot be determined.
 *
 * Must be called once the device holds an IP address (e.g. from the
 * `IP_EVENT_*_GOT_IP` handler); calling it earlier races the netif bring-up.
 * Repeated calls are ignored, so it is safe to wire into a got-IP handler that
 * also fires on DHCP renewal or link flap.
 *
 * @param[in] server_ip  IPv4 address of the syslog server in dotted-quad form.
 *                       NULL, empty or unparseable disables remote logging.
 * @param[in] port       Destination UDP port (typically 514). 0 disables
 *                       remote logging.
 * @param[in] hostname   Value for the RFC3164 HOSTNAME field. Spaces are
 *                       replaced with underscores; NULL or empty keeps the
 *                       built-in default.
 */
void net_syslog_start(const char *server_ip, uint16_t port, const char *hostname);

#ifdef __cplusplus
}
#endif
