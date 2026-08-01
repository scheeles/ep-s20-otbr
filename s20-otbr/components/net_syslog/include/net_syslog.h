#pragma once

#include <stdint.h>

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief Start mirroring the ESP-IDF log stream to a remote UDP syslog server.
 *
 * Installs a chained `esp_log_set_vprintf()` hook: every log line is still
 * handed to the previously installed handler first (so UART/console output and
 * the web log buffer keep working exactly as before), then formatted and handed
 * to a worker task that owns the UDP socket and emits the RFC3164 packet.
 *
 * The hook itself never calls into lwip. That is deliberate: the log hook runs
 * on whichever task logged — including lwip's own tcpip thread — and a socket
 * call from there would deadlock the network stack. Queueing keeps the logging
 * path free of lwip entirely, and a full queue drops lines rather than delaying
 * the caller.
 *
 * The syslog PRI is derived from the ESP-IDF level letter of each line
 * (facility `local0`): `E` -> err, `W` -> warning, `I` -> info, `D`/`V` ->
 * debug, defaulting to info when the level cannot be determined.
 *
 * The socket and the hook are set up even when no server is configured, so the
 * destination can be supplied later at runtime via net_syslog_set_server()
 * without a reboot. Until then nothing is queued, and neither the queue nor the
 * worker task is created.
 *
 * Must be called once the device holds an IP address (e.g. from the
 * `IP_EVENT_*_GOT_IP` handler); calling it earlier races the netif bring-up.
 * Repeated calls are ignored, so it is safe to wire into a got-IP handler that
 * also fires on DHCP renewal or link flap.
 *
 * @param[in] server_ip  Initial IPv4 address of the syslog server in
 *                       dotted-quad form. NULL or empty starts dormant.
 * @param[in] port       Initial destination UDP port (typically 514). 0 starts
 *                       dormant.
 * @param[in] hostname   Value for the RFC3164 HOSTNAME field. Spaces are
 *                       replaced with underscores; NULL or empty keeps the
 *                       built-in default. Only read here, so a later change
 *                       takes effect on the next boot.
 */
void net_syslog_start(const char *server_ip, uint16_t port, const char *hostname);

/**
 * @brief Point remote logging at a different server, or switch it off.
 *
 * Safe to call at any time, including before net_syslog_start() (the value is
 * remembered) and while other tasks are logging: the destination is published
 * to the worker without locking and without touching the socket, so no line can
 * ever observe a half-written address.
 *
 * The first call with a valid server also creates the queue and worker task, so
 * a device that never configures a server pays no RAM for them.
 *
 * @param[in] server_ip  IPv4 address in dotted-quad form. NULL or empty stops
 *                       sending until a server is configured again.
 * @param[in] port       Destination UDP port. 0 stops sending.
 * @return
 *   - ESP_OK                on success, including when disabling
 *   - ESP_ERR_INVALID_ARG   when server_ip is not a valid IPv4 address
 *   - ESP_ERR_NO_MEM        when the queue or worker task could not be created
 */
esp_err_t net_syslog_set_server(const char *server_ip, uint16_t port);

#ifdef __cplusplus
}
#endif


#ifdef __cplusplus
}
#endif
