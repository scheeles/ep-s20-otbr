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

To flash the firmware with a USB cable:
```
$ idf.py -p /dev/ttyUSB0 flash
```
This leaves the `nvs` partition alone, so the Thread dataset and settings survive.

To erase the flash completely (only needed when the partition layout changes, or to
factory reset — this drops the Thread commissioning):
```
$ idf.py -p /dev/ttyUSB0 erase-flash
```

### Partition layout

| partition | offset | size |
| --- | --- | --- |
| nvs | `0x9000` | 64 K |
| otadata | `0x19000` | 8 K |
| phy_init | `0x1B000` | 4 K |
| ota_0 | `0x20000` | 4 M |
| ota_1 | `0x420000` | 4 M |
| web_storage | `0x820000` | 500 K |
| rcp_fw | `0x89D000` | 640 K |

**Never assume these offsets for a device in front of you.** OTA updates only replace
partition *contents*, never the partition table, so a device that has been updated
over the network keeps whatever layout it was originally flashed with. Writing an
image at the wrong offset corrupts whichever partition actually lives there — a
mangled `rcp_fw` shows up as `SPIFFS: mount failed` and a boot loop.

The authoritative layout is the one the device prints on every boot:
```
$ idf.py -p /dev/ttyUSB0 monitor
...
I (37) boot: Partition Table:
I (40) boot:  0 nvs              WiFi data        01 02 00009000 00010000
...
```
Use those offsets when flashing individual binaries with `esptool write-flash`.
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

The device has no serial tether in production, so the `net_syslog` component mirrors
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

### Setting the server

The firmware ships with remote logging compiled in but **dormant**. Set the server
under **Network → Remote Logging** in the web UI; it is stored in NVS and applied
immediately, with no reboot and no rebuild. Clearing the server field switches
remote logging back off. This is what lets several border routers run one identical
firmware image and still log to different collectors.

The RFC3164 HOSTNAME field follows the device hostname (**Network → Hostname**, the
same value used for mDNS), so hosts stay distinguishable in Loki. Unlike the server,
a hostname change applies on the next reboot.

The same settings are reachable over the API:

```
$ curl http://<device>/config
$ curl -X PUT http://<device>/config -d '{"syslog_srv":"192.168.1.10","syslog_port":"514"}'
$ curl -X PUT http://<device>/config -d '{"syslog_srv":""}'      # switch off
```

### Build-time defaults

The Kconfig options under **Network syslog** only supply the fallback used when
nothing is stored in NVS — a device that has never been configured:

| Option | Default | Meaning |
| --- | --- | --- |
| `CONFIG_NET_SYSLOG_ENABLED` | `y` (via `sdkconfig.defaults`) | Compile the component in and expose the UI card |
| `CONFIG_NET_SYSLOG_SERVER_IP` | `""` | Fallback server; empty means dormant until set in the UI |
| `CONFIG_NET_SYSLOG_SERVER_PORT` | `514` | Fallback UDP port |
| `CONFIG_NET_SYSLOG_HOSTNAME` | `ep-s20-otbr` | Fallback HOSTNAME when none is stored |

Setting `CONFIG_NET_SYSLOG_SERVER_IP` is only useful to pre-provision a fleet that
should start logging on first boot.

### Notes

The component is started from the got-IP handler in
[`main/esp_ot_br.c`](s20-otbr/main/esp_ot_br.c) — never earlier, or the socket setup
races the netif. The call is idempotent, so DHCP renewals re-entering the handler
will not undo a server configured from the UI.

**The log hook never calls into lwip.** It formats the line and drops it into a
fixed-size queue; a dedicated worker task owns the socket and sends the packet. This
is not an optimisation — the hook runs on whichever task logged, *including lwip's
own tcpip thread*, and with `LWIP_TCPIP_CORE_LOCKING` disabled (the ESP-IDF default)
a socket call from that thread waits on a semaphore only that thread can signal:
an unrecoverable deadlock of the whole network stack. A non-blocking socket does not
help, because `O_NONBLOCK`/`MSG_DONTWAIT` only govern what lwip does *after* the
tcpip thread picks the message up.

Costs of that split:

| | |
| --- | --- |
| Stack borrowed from a logging task | ~370 B (one `SYSLOG_LINE_MAX` buffer) |
| Queue | 16 × 256 B = 4 kB |
| Worker task stack | 3.5 kB |

The queue and worker are created on the **first** successful server configuration, so
a device that never enables remote logging pays nothing. A full queue drops lines
rather than delaying the caller, so a burst costs log lines, never latency.

To verify on the receiving host:

```
$ nc -u -l -p 514
$ tcpdump -nAi any udp port 514
```

## Integrating with Home Assistant

See [Home Assistant](docs/homeassistant.md).