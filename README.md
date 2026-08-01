# GL-S20 OpenThread Border Router

[GL's S20](https://www.gl-inet.com/products/gl-s20) is a nice and inexpensive device but firmware support is lacking behind as GL's attention is on more recent hardware.
Luckily enough, they shared an [OpenSDK](https://github.com/gl-inet/s20_thread_br_opensdk) that I forked to upgrade things a bit.

This is a minimalist, performance oriented firmare that supports:

- [esp-idf](https://github.com/espressif/esp-idf) -> v6.0.2
    - Thread v1.4
    - TREL support
    - BBR support
    - Home Assistant OTBR integration
- [esp-thread-br](https://github.com/espressif/esp-thread-br) -> main @ 25ab204
    - Used as base framework
    - Implemented only wired connectivity in order to keep the radio for Thread use only
    - Forked basic Web UI to add logs, remote/local OTA and more
- [s20_thread_br_opensdk](https://github.com/gl-inet/s20_thread_br_opensdk) -> main @ 2b610f8
    - Leveraged primarily for LED support, PIN layout and base IDF settings

**Use at your own risk!** (but you can always flash back the original firmware...)

## Getting started

```
git clone --recursive https://github.com/epinci/ep-s20-otbr.git
git submodule update --init --recursive
```

Install SDK environment and build RCP firmware:
```
./utils/install-idf.sh
```

Activate SDK environment:
```
. ./esp-idf/export.sh
```

Build S20 firmware:
```
cd s20-otbr
idf.py set-target esp32s3
idf.py build
```

First deployment must use a USB connection, subsequent updates can be pushed by network.
If you're using WSL, check [Connect USB devices to WSL](https://learn.microsoft.com/en-us/windows/wsl/connect-usb)

To erase the flash with a USB cable (recommended before flashing the firmware):
```
$ idf.py -p /dev/ttyUSB0 erase-flash
```
To flash the firmware with a USB cable:
```
$ idf.py -p /dev/ttyUSB0 flash
```
To start log monitoring with a USB cable:
```
$ idf.py -p /dev/ttyUSB0 monitor
```
To exit monitor use `Ctrl + ]` or `CTRL + T, CTRL + X`

After the first flash you can push a build with:
```
$ ./script/push-update.sh <IP-ADDRESS>
```

More [flashing procedures](docs/moreflash.md).

## Remote logging (syslog)

The device has no serial tether in production, so the `net_syslog` module mirrors
the ESP-IDF log stream to a remote **RFC3164 UDP syslog** server (e.g. a Grafana
Alloy `loki.source.syslog` listener). It chains onto the existing
`esp_log_set_vprintf()` handler, so **UART/console logging is unchanged** — syslog
is purely additive. Sends are best-effort and non-blocking on a cached UDP socket;
if the network is down or the queue is full, lines are silently dropped.

Each packet looks like:

```
<134>Jan  1 00:00:00 ep-s20-otbr otbr: I (5213) s20-otbr: Ethernet Got IP Address
```

The PRI encodes facility `local0` plus the severity mapped from the ESP-IDF level
letter (`E`→err/131, `W`→warning/132, `I`→info/134, `D`/`V`→debug/135), so the
receiver can label severity. ANSI colour escapes are stripped. The device clock is
usually wrong — the receiver should stamp its own arrival time.

Enable it in `idf.py menuconfig` under **Network syslog Configuration**:

| Option | Default | Meaning |
| --- | --- | --- |
| `CONFIG_NET_SYSLOG_ENABLED` | `n` | Master switch for remote logging |
| `CONFIG_NET_SYSLOG_SERVER_IP` | `192.168.1.10` | Syslog server IPv4 (dotted-quad, no DNS) |
| `CONFIG_NET_SYSLOG_SERVER_PORT` | `514` | Destination UDP port |
| `CONFIG_NET_SYSLOG_HOSTNAME` | `ep-s20-otbr` | RFC3164 HOSTNAME fallback |

or in `sdkconfig.defaults`:

```
CONFIG_NET_SYSLOG_ENABLED=y
CONFIG_NET_SYSLOG_SERVER_IP="192.168.1.10"
CONFIG_NET_SYSLOG_SERVER_PORT=514
CONFIG_NET_SYSLOG_HOSTNAME="ep-s20-otbr"
```

The HOSTNAME field prefers the hostname stored in NVS (the same one used for mDNS,
settable from the web UI) and falls back to `CONFIG_NET_SYSLOG_HOSTNAME`, so several
border routers stay distinguishable without per-device builds.

It is started from the got-IP handler in [`main/esp_ot_br.c`](s20-otbr/main/esp_ot_br.c)
— never earlier, or the socket setup races the netif:

```c
net_syslog_start(CONFIG_NET_SYSLOG_SERVER_IP, CONFIG_NET_SYSLOG_SERVER_PORT, hostname);
```

The call is idempotent, so DHCP renewals re-entering the handler are harmless.

The hook formats on the caller's stack (bounded buffers, no heap): a logging task
needs roughly 1.1 kB of headroom. Shrink `SYSLOG_MSG_MAX` / `SYSLOG_PKT_MAX` in
[`main/net_syslog.c`](s20-otbr/main/net_syslog.c) if a task with a tight stack has
to log.

To verify on the receiving host:

```
$ nc -u -l -p 514
$ tcpdump -nAi any udp port 514
```

## Integrating with Home Assistant

See [Home Assistant](docs/homeassistant.md).