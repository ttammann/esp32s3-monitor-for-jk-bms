# jk-bms-monitor

ESP-IDF firmware for the Waveshare ESP32-S3-RS485-CAN, reading one JK inverter
BMS (JK-PB2A16S20P, hw V19A / sw V19.31) and publishing to MQTT with Home
Assistant discovery.

**One protocol: JK's 55AA push stream.** The BMS at DIP `0000` masters its own
RS485-P bus, scanning addresses `00`–`0F` and answering itself at address 0. We
only listen. **Read-only is structural, not a policy**: the UART is opened with
no TX pin and DE is held low as a plain GPIO, so this firmware cannot transmit.

**Data arrives every ~6.4 s**, set by the BMS's scan cycle (16 addresses × 2
requests × 200 ms) — not at 1 Hz. Size every timeout against that.

Two frames arrive per cycle, and both are decoded:

| Frame | Byte 4 | Answers | Carries |
|---|---|---|---|
| DYNAMIC | `0x02` | a `0x1620` write | voltages, current, power, SOC/SOH, temps, cells, cycles |
| SETTINGS | `0x01` | a `0x161E` write | OVP/UVP thresholds, current limits, capacity, switches |

See [`jk-bms-esp32s3-firmware-plan.md`](jk-bms-esp32s3-firmware-plan.md) for the full plan and
[`jk-bms-build-checklist.md`](jk-bms-build-checklist.md) for the phase-by-phase checklist. The vendor
datasheet is the [JK-PB2A16S20P BMS Specification V1.0](https://www.gobelpower.com/download/JKBMS-JK-PB2A16S20P-Specication-EN-V1.0.pdf).

## Configure

**All settings are build-time.** They are compiled into the binary, so the
order is: configure, *then* build, *then* flash. Changing a setting after
flashing does nothing until you rebuild and reflash.

```
idf.py menuconfig     # -> "JK BMS Monitor Configuration"
```

Three settings ship as `SET_ME` placeholders — Wi-Fi SSID, Wi-Fi password and
the MQTT broker URI — and the firmware checks all three at boot, logging an
explicit error naming each one still unset. Otherwise a stale placeholder
surfaces as "Haven't to connect to a suitable AP" and reads like a network
fault rather than a configuration one. The password is checked but never
printed.

Leave MQTT username and password empty for an anonymous broker — the client
then connects with no credentials rather than authenticating as an empty user,
which brokers reject with a misleading "not authorized". They are deliberately
*not* placeholder-checked: empty is a valid configuration, not an unset one.

### Keeping credentials out of the tree: `overlay/`

Put your credentials in `overlay/sdkconfig.secrets` (the `overlay/` folder
is gitignored):

```
CONFIG_JK_WIFI_SSID="my-network"
CONFIG_JK_WIFI_PASS="..."
CONFIG_JK_MQTT_URI="mqtt://192.168.1.50:1883"
CONFIG_JK_MQTT_USER="jkbms"
CONFIG_JK_MQTT_PASS="..."
```

When that file exists, `CMakeLists.txt` seeds the config from it and keeps the
generated `sdkconfig` in `overlay/` as well, so nothing secret is written to the
project folder. `overlay/sdkconfig` wins once it exists: after editing the
secrets file, delete `overlay/sdkconfig` and run `idf.py reconfigure` (or just
use `menuconfig`). Without an overlay, `sdkconfig` lands in this folder as
usual, and is gitignored.

> **Where your secrets land once you set them:** `sdkconfig`,
> `build/config/sdkconfig.h`, and the flashed `.bin` — all gitignored. Treat
> the binary as sensitive — don't hand someone a build artifact expecting the
> password not to be in it. The transport is `mqtt://`, so credentials also
> cross the LAN in the clear.

## Build / flash

```
idf.py -p /dev/cu.usbmodemXXXX flash monitor
```

There is no separate sniffer build. The listener runs with or without Wi-Fi:
it is started before the network and never waits on it, so RS485 bring-up works
with the credentials still at `SET_ME`, and MQTT publishes are no-ops until the
broker connects.

Real output from this unit:

```
I jk: DYNAMIC len=300 (sum8)  52388 mV  -1615 mA  84606 mW  soc 62% soh 100%
I jk:      cells=16 present=0x0000FFFF avg=3274mV delta=2mV max=#4 min=#1
I jk:      mos=28.6C t1=26.2C t2=26.4C bal=0mA alarms=0x00000000 chg=1 dsg=1 bal=0
I jk:      remain=194320mAh full=314000mAh cycles=11 cycle_cap=3527140mAh runtime=6321223s
        cell 01-08: 3273 3274 3274 3275 3275 3275 3275 3274 mV
        cell 09-16: 3274 3275 3275 3274 3275 3274 3274 3274 mV
```

Settings frames arrive just as often but change almost never, so they are
logged once and then only when something actually differs.

## Frame format

**300 bytes, additive `sum8`**, followed by a *separate* frame-request write.
The vendor-adjacent doc describing a "308-byte frame with a trailing CRC16" was
counting both as one; it is really two things on the wire, and the demux reports
them as two. The same doc claims SETTINGS frames carry 24 extra tail bytes — on
this unit they do not, and are plain 300-byte sum8 frames like DYNAMIC.
`jk55.c` still accepts a 308-byte CRC16 variant for other firmware revisions,
but it has never been seen here.

**Byte 4 types the frame** — see the long note in `jk55.h`. The vendor doc calls
it a sequence number and the reference Python implementation uses it as a pack
id; on this single-pack bus it is nonetheless an exact discriminator, because
the BMS issues its two request registers at a strict 1:1 ratio and each value
pairs with the request that provoked it. Measured over 220 s: 559 × `0x161E`
and 559 × `0x1620`, answered by 35 SETTINGS and 35 DYNAMIC frames — 16
addresses × 35 scan cycles. Add a second pack and typing has to move to the
request register.

## Bus health

Every 10 s the listener reports byte, frame, drop and UART-error counts, plus a
verdict. "Nothing happening" is several different faults needing opposite fixes,
so they are named rather than merged:

| Verdict | Means |
|---|---|
| `SILENT` | no bytes, no errors — wiring, DIP, or a sleeping BMS |
| `NOISE` | errors dominate — A/B swapped, or wrong baud |
| `UNFRAMED` | clean bytes, nothing parses — unknown protocol |
| `OVERRUN` | dropping data; other counts unreliable |
| `NO_DATA` | BMS is scanning, but no pack answered |
| `OK` | 55AA frames decoding, with the measured DYNAMIC interval |

`NO_DATA` is the one that used to be missing. Frame-request writes are not
battery data, and counting them as frames reported a healthy link on a bus where
nothing was answering.

## What is not here

**The Modbus RTU poller was removed.** It was a hypothesis-stage register map
that was never validated, and the 55AA path closed Phase 1 without it. Deleted
with it: the Modbus master, the register map and its decoder, the slave-address
and poll-interval settings, and the general RTU frame parser.

That parser is worth a note, because removing it fixed a real bug. For function
codes it did not model it brute-forced a CRC match across every length from 4 to
256. On noise that fabricated a "valid" frame roughly **every 588 bytes** —
enough to make the bus verdict read `OK` on a dead bus, and occasionally long
enough to swallow the start of a real 55AA frame (measured: 4 frames lost in 200
at 5 stray bytes per scan cycle). It also cost ~13,500 CRC byte-operations per
received byte, which an ESP32-S3 cannot sustain at 115200 baud.

What replaced it (`jk_req.c`) recognises a closed set: two register addresses,
two function codes, sixteen pack addresses. It cannot fabricate a frame, and a
test feeds it 200,000 random buffers to keep it that way. On this bus the
measured `dropped` count is **0**.

## Host tests

```
sh test/run.sh
```

No ESP-IDF, no hardware. Covers the 55AA framer and both decoders (`jk55.c`),
the CRC and frame-request recogniser (`jk_crc.c`, `jk_req.c`), the protocol
demultiplexer (`jk_demux.c`), the bus diagnosis (`jk_busdiag.c`), the
console/JSON formatting (`jk_fmt.c`) and the bounded string builder
(`strbuf.h`) — all deliberately free of ESP-IDF dependencies so they compile on
the Mac. Built with ASan + UBSan and `-Wconversion -Werror`; set `NOSAN=1` to
skip the sanitizers.

`jk_fmt.c` exists so the indexing is testable: the cell-row and JSON builders
index arrays with values taken straight off the wire, and used to live inside
the listener where they could not be compiled on the host.

## Topics

| Topic | Retained | Meaning |
|---|---|---|
| `jkbms/<id>/status` | yes | `online`/`offline` — firmware alive (LWT) |
| `jkbms/<id>/state` | no | full JSON snapshot, one per DYNAMIC frame |
| `jkbms/<id>/settings` | yes | pack configuration, on change and on reconnect |
| `jkbms/<id>/bms_status` | yes | `online`/`offline` — frames arriving |
| `homeassistant/sensor/<id>_*/config` | yes | HA discovery |
| `homeassistant/binary_sensor/<id>_*/config` | yes | HA discovery |

`bms_status` follows the *data*, not the link: it goes offline when no DYNAMIC
frame has arrived for 25 s (four missed cycles). A listener with a live UART and
a silent BMS has nothing to publish and says so. The **BMS comms** binary sensor
reads this topic directly rather than a field in the state JSON — a freshness
flag computed while building a state payload is true by construction, so it
could never report the disconnection it exists to report.

State entities require **both** status topics online (`avty_mode: all`), so a
dead RS485 link shows as *unavailable*, never as stale-but-plausible numbers.
They also carry `expire_after` (`CONFIG_JK_EXPIRE_S`, default 30 s) — the
availability topics catch a clean disconnect and the LWT, but only expiry
catches a publisher that wedges mid-loop.

**Settings entities deliberately carry neither.** They publish only when the
configuration changes, so expiry would blank them during normal operation. That
is also why the settings payload is republished on every MQTT reconnect and
retried until it is actually accepted: the listener starts before the network,
so the first settings frame would otherwise be published into a disconnected
client and — being identical to every frame after it — never sent again.

Watch the broker from the Mac:

```
mosquitto_sub -h <broker> -u <user> -P <password> -v -t 'jkbms/jkbms-aabbcc/#'
```
