# JK BMS (v19) monitor — Waveshare ESP32-S3-RS485-CAN firmware plan

**Date:** 2026-08-29
**Target:** Waveshare ESP32-S3-RS485-CAN (DIN rail, ~$27)
**Goal:** Poll one JK inverter BMS (hardware V19) over RS485 Modbus, publish to MQTT with Home Assistant discovery.

## Decisions locked in

| Question | Answer |
|---|---|
| Framework | ESP-IDF 5.x (C), FreeRTOS tasks, native drivers |
| Output | MQTT + Home Assistant discovery |
| Protocol | **JK 55AA push stream** (was: Modbus RTU — see "Protocol decision") |
| Bus topology | **BMS masters its own bus; we are a pure listener** (was: ESP32 as sole master) |
| Data cadence | **~6.4 s per DYNAMIC frame**, set by the BMS scan cycle (was: 1 Hz polling) |
| Power | Separate always-on supply (independent of the pack) |
| Write/control | **Out of scope** — read-only, now enforced by a build with no TX pin. |

> **Two rows changed on 2026-08-31** after the first live capture. The plan was written around
> polling Modbus as sole master at 1 Hz. The BMS turns out to poll itself, so we listen instead,
> and the data arrives at its cadence rather than ours. Details in "Protocol decision" and
> "Findings from the first capture".

### Confirmed unit (read off the JK app, 2026-08-30)

| | |
|---|---|
| Vendor ID | **JK-PB2A16S20P** |
| Hardware version | V19A |
| Software version | **V19.31** |
| Hardware option | CEHMPRT |
| App version | V5.12.0 |
| First powered on | 2026-04-21 |

Two consequences worth pulling forward:

- **The V19 DIP bug applies to this unit.** Section 1 warns that on firmware ≥ V19.10 the DIP
  address does not reliably auto-configure the port protocol. V19.31 is well past that line,
  so re-checking the protocol field after every power cycle is mandatory here, not optional.
- **The exact model is documented upstream.** `syssi/esphome-jk-bms` carries a
  `docs/pb2a16s20p/` directory plus an "RS485 Communication example.pdf" — vendor material for
  precisely this model, which is the authority the register table below should be validated
  against before any live capture.

---

## 1. Hardware

### Board pinout

| Function | GPIO |
|---|---|
| RS485 TX | 17 |
| RS485 RX | 18 |
| RS485 DE/RE ("talk" pin) | 21 |
| CAN TX / RX | 15 / 16 (unused) |

The board is documented as having "automatic" transceiver switching, but every working
integration still drives GPIO21 explicitly. In ESP-IDF this is free: put UART1 into
`UART_MODE_RS485_HALF_DUPLEX` and assign GPIO21 as the RTS pin — the driver raises and
lowers DE around each transmission with correct timing.

```c
uart_set_pin(UART_NUM_1, GPIO_NUM_17, GPIO_NUM_18, GPIO_NUM_21, UART_PIN_NO_CHANGE);
uart_set_mode(UART_NUM_1, UART_MODE_RS485_HALF_DUPLEX);
```

**Verified on the bench, 2026-08-31** (`esptool.py -p /dev/cu.usbmodem1101 flash_id`):

| | |
|---|---|
| Chip | ESP32-S3 (QFN56) revision **v0.2** |
| Flash | **16 MB**, quad SPI, 3.3 V (set in eFuse) |
| PSRAM | **8 MB** embedded (AP_3v3) — present, deliberately not enabled |
| USB | USB-Serial/JTAG (native; no bridge chip, no driver) |
| MAC | `xx:xx:xx:aa:bb:cc` → device id `jkbms-aabbcc` |
| Crystal | 40 MHz |

`sdkconfig.defaults` has been corrected from the 4 MB placeholder to 16 MB. That is not
cosmetic: Phase 5's OTA-with-rollback needs two app partitions, and a 4 MB declaration would
have capped the partition table before that became possible.

**Still to verify on the bench:**
- Whether USB-C 5V alone powers the *isolated* RS485 side, or whether that rail only comes
  up from the 7–36V DC terminal. If it's the latter, the "always-on supply" must be a 12V
  adapter into the screw terminal, not USB-C. (The Waveshare wiki 403s; this needs a meter.)

### Wiring to the BMS

JK inverter BMS RS485-1 is an RJ45; RS485-2 is a 4-pin connector. Either works — pick the one
the inverter is *not* using.

| Signal | RS485-1 (RJ45) | RS485-2 (4-pin) | Board terminal |
|---|---|---|---|
| A (+) | pins 2 and 7 | pin 2 | A |
| B (−) | pins 1 and 8 | pin 1 | B |
| GND | pins 3 and 6 | pin 3 | **no terminal — see below** |

> **Corrected 2026-08-31.** This section previously said "the signal ground is not optional —
> the isolated transceiver needs a reference", and listed a `GND` board terminal. Both were
> wrong. The ESP32-S3-RS485-CAN's RS485 screw terminal is **A+ / B− only**; Waveshare does not
> bring the isolated-side ground out at all. Two-wire is the intended use of this board.
>
> The "signal ground is mandatory" rule applies to *non-isolated* transceivers on long
> multi-node buses. With galvanic isolation and two nodes on a short cable, the field side
> floats and self-references to the bus common-mode — that is what the isolation is for.
>
> **Do not improvise a ground** by landing BMS negative on the 7-36V power terminal's GND or
> on a header pin. That bridges the isolation barrier and bonds battery negative to the board
> ground, and through USB to the laptop. The absence of an RS485 GND terminal is what keeps a
> 48V-referenced ground out of your bench; leave it that way.

- Use one twisted pair for A/B (T568B orange pair, RJ45 pins 1-2). Nothing else is landed.
- Enable the board's 120Ω termination jumper (120R position). With only two nodes, the ESP32
  is a bus end.
- Keep the RS485 GND bonded only to the BMS. Do not tie it to the auxiliary supply's ground;
  the board's isolation is what keeps a 48V-referenced BMS ground out of your bench supply.

### BMS-side configuration (do this before writing any code)

1. Set the chosen port's protocol to **`001 - JK BMS RS485 Modbus V1.0`** (there is also
   `013 - (9600) JK BMS RS485 Modbus V1.0` if you want 9600 baud instead of 115200).
2. Set the DIP address to a **non-zero** value, e.g. `0x01`. Address `0x00` puts UART2 into
   *master* mode, which will fight your poller.
3. **Known V19 bug:** on firmware ≥ V19.10, setting a DIP address of 1–15 does not reliably
   auto-configure the UART2/RS485-2 port protocol. After setting the DIP, re-open the app and
   confirm the protocol field actually reads `001`. Re-check after any BMS power cycle.

---

## 2. Protocol layer

> ### ⚠️ Sections 2.1 and 2.4 are superseded — kept as a record, not as instructions
>
> **Updated 2026-08-31.** The Modbus register map and the RTU framing rules below describe
> code that **no longer exists**. Track A (55AA) closed Phase 1 on its first attempt and the
> Modbus poller was deleted — `mbrtu.c`, `mb_frame.c`, `jk_regs.h` and `jk_decode.c` are all
> gone, along with the slave-address and poll-interval settings.
>
> They are left in place because "Protocol decision" and "Findings" below only make sense
> against what they replaced. **Do not implement from them.** For the protocol as actually
> built, read `jk-bms-monitor/README.md` and the header comments in `jk55.h` / `jk_req.h`.

### 2.1 Modbus RTU — superseded, never validated

Modbus RTU, 115200 8N1 (or 9600 with protocol 013).

- **0x03** — read holding registers (everything we need)
- **0x10** — write multiple registers (not used; see exclusions)
- CRC16, poly `0xA001`, low byte first

The register table below was a hypothesis assembled from community reimplementations, not
from the vendor doc. **It was never validated against hardware and never will be** — nothing
reads it. It is recorded only to show the scale of the archaeology that taking Track A
avoided.

| Field | Address | Type | Scale |
|---|---|---|---|
| Pack voltage | `0x0090` | u32 | mV |
| Pack current | `0x0098` | i32 | mA (sign = charge/discharge) |
| SOC | `0x00A6` | u8 in 2 bytes | % |
| Cell voltages 0–31 | `0x1200`–`0x123E` | u16 each | mV |
| Temperatures | `0x008A`, `0x009C`, `0x009E`, … | i16 | 0.1 °C |

The one part of this stack that survived is the CRC-16/MODBUS routine, now `jk_crc.c` — the
55AA bus borrows the same checksum for the frame-request writes. The name is the algorithm's;
nothing in the firmware speaks Modbus any more.

### 2.2 Protocol decision — RESOLVED 2026-08-31, Track A

**Track A (55AA) worked on the first attempt. Track B is abandoned.** The Modbus register
archaeology this document called "the single largest unknown in the project" is not needed:
`jk_regs.h` never has to be validated, because nothing reads it any more.

The two options as they were assessed, with the outcome:

| | **Modbus RTU (protocol 001)** | **55AA push stream** |
|---|---|---|
| Port | RS485-1/-2, DIP address 1–15 | RS485B / RS485C, DIP `0000` |
| Direction | ESP32 polls as master | BMS transmits unprompted |
| Register map | Hypothesis, unvalidated | Documented offset-by-offset |
| Fields | ~8 | ~40, incl. SOH, cycles, capacity, alarms, wire resistance |
| Needs to transmit? | Yes | **No** |
| Status *at the time of the decision* | `mbrtu` + `jk_decode`, wired to MQTT | `jk55`, sniffer only |
| Status **now** | deleted | the whole firmware |

Both open questions were answered by one 11-second capture:

- **Does V19.31 push without a request?** Effectively yes, but not for the reason the docs
  give. See "Findings" below — the BMS is generating the requests itself.
- **Which integrity check?** **300-byte frames with an additive `sum8`.** The 308-byte
  CRC16 variant was never seen; `JK55_CK_CRC16` stays in the code for other firmware but is
  unexercised on this unit.

Prior art confirming this is viable on this exact model: `jean-luc1203/jkbms-rs485-addon`
lists PB2A16S20P explicitly for firmware 14/15/19, and offers a **broadcast mode where one JK
BMS acts as master and the addon listens passively** — the same topology proposed here.
Note that `syssi/esphome-jk-bms` is *not* prior art for this path: it implements UART-TTL and
BLE only, with no RS485 Modbus for the PB series. Its value to us is its documentation folder,
not its code.

Two questions were open against Track B when this was written. Both are now moot, and are
recorded because they are the cost Track A avoided:

- ~~**Word order of 32-bit values.**~~ Big-endian words vs. swapped, to be determined by
  whether a 53.2 V pack decoded to something absurd. Never needed: the 55AA protocol is
  little-endian throughout, `le16`/`le32` in `jk55.c`, confirmed by five independent
  cross-checks on the first captured frame. The `JK_WARN_PACK_RANGE` plausibility flag
  mentioned here belonged to the deleted decoder and no longer exists.
- ~~**Whether the bulk-read path is worth it.**~~ Writing `0x161C`–`0x1624` to trigger block
  downloads is, as it turns out, exactly what the BMS already does to itself — see Findings
  below. We read the results without asking.

### 2.3 Findings from the first capture (2026-08-31)

Captured with the listen-only build on **RS485-P, DIP `0000`**, PB2A16S20P sw V19.31.

**1. The BMS masters its own bus.** The wire is not quiet-until-pushed. The BMS writes
`0x1620` (dynamic) and `0x161E` (settings) via Modbus function `0x10` to addresses `00`
through `0F`, 200 ms apart, then repeats. It finds *itself* at address 0 and answers; `01`–`0F`
go unanswered because there are no other packs. That is the real mechanism behind the vendor
doc's claim that the BMS "actively and automatically sends data without needing a request" —
the requests exist, the BMS just makes them.

The consequence is architectural and permanent: **we never transmit, and never need to.** This
is a stronger read-only guarantee than the original design, which was a master that chose not
to issue writes. The sniffer build has no TX pin routed at all.

**2. The frame is 300 bytes + a separate 8-byte Modbus echo.** The source document describes a
"308-byte frame with a trailing Modbus CRC16". On the wire it is a 300-byte 55AA frame
validated by `sum8`, immediately followed by a distinct 8-byte Modbus `0x10` echo from the
addressed pack. Two frames, not one. The demux reports them separately, which is correct.

**3. Cadence is ~6.4 s, and it is the BMS's number, not ours.** 16 addresses x 2 requests x
200 ms = 6.4 s per full scan, giving one DYNAMIC frame per cycle. Every downstream timeout —
`expire_after`, staleness, watchdogs — must be sized against this, not against the 1 Hz the
original polling design assumed. The listener measures the interval and prints it in the 10 s
bus report rather than trusting the arithmetic. (`sniff.c`, named here originally, was folded
into `listen.c` when the sniffer stopped being a separate build.)

**4. The decode is validated.** First frame, cross-checked five independent ways:

| Check | Result |
|---|---|
| Sum of 16 cell voltages vs reported pack voltage | 52390 vs 52388 mV |
| V x I vs reported power | 84.6 vs 84.606 W |
| remaining/full capacity vs reported SOC | 61.9% vs 62% |
| decoded runtime vs the JK app's "Total Time" | 73.16 days vs 72D15H20M |
| computed cell delta vs reported delta | 2 vs 2 mV |

These come from unrelated byte offsets, so agreement is evidence the offsets are right, not an
artefact of the arithmetic.

**5. What this unlocks.** ~40 fields instead of ~8: SOH, remaining and full capacity, cycle
count, total cycle capacity, runtime, balance current, the alarm bitfield, charge/discharge/
balance MOS states, the cell-presence bitmap, and 16 wire resistances. **All of it is now
wired to MQTT** (updated 2026-08-31) — see §4, and `ha_discovery.c` for the entity list. The
SETTINGS frame was decoded afterwards and adds the configured thresholds and limits on top.

### 2.4 Framing rules — superseded, describes deleted code

> **Superseded 2026-08-31.** Everything below governs transmitting as a Modbus master:
> `mb_wait_t35()`, the retry ladder, the stale-after-5-failures rule. **None of it exists.**
> This firmware never transmits, so there is no t3.5 to enforce, no retry policy and no poll
> cycle to fail. Staleness is now a single timer on the arrival of DYNAMIC frames
> (`LS_STALE_AFTER_US`, 25 s ≈ four missed 6.4 s cycles) published as `bms_status`.
>
> Kept because the t3.5 correction below is a genuinely useful piece of Modbus knowledge that
> was hard-won, and it applies again the moment anyone adds a write path.

> **Corrected 2026-08-30.** This section previously said inter-frame silence was "~35 µs at
> 115200 — not enforceable from an RTOS task, and you don't need to as the sole master."
> Both halves were wrong. 3.5 character times at 115200 is **~334 µs** (a character is 11 bit
> times, not 1), and the spec **fixes t3.5 at 1.75 ms** for any baud above 19200 rather than
> scaling it. More importantly, being the sole master does not exempt you: the silence is what
> the *slave* uses to detect end-of-frame. Send the next request too soon after the previous
> response and the BMS can read it as a continuation of the previous frame and discard both.
> It is enforceable — `esp_rom_delay_us()` from the task, measured against `esp_timer`, since
> the wait is sub-millisecond and the tick is 10 ms.

- Enforce t3.5 of bus silence before every transmission, measured from the last byte seen in
  either direction. Implemented as `mb_wait_t35()` in `mbrtu.c`.
- Transmit, then `uart_read_bytes()` with a 300 ms timeout, reading the expected byte count
  derived from the request.
- Validate length, slave address, function code, then CRC. Any mismatch → flush RX, retry.
- 3 attempts with a 200 ms gap. **A Modbus exception is not retried** — a healthy slave
  rejecting a register will reject it three times, so retrying only slows down the Phase 1
  feedback loop. Timeouts, bad CRCs and malformed headers are retried.
- After 5 consecutive failed poll cycles, mark data stale.

**Timing consequence:** with retries, a fully dead bus costs up to 3 × 300 ms + 2 × 200 ms =
1.3 s per read, and there are two reads per cycle. A dead BMS therefore takes ~13 s to go
stale rather than ~5 s. That is the intended trade — a single dropped byte no longer costs a
whole poll cycle.

---

## 3. Firmware architecture

**As built, 2026-08-31.** The tree below is what exists, verified against the source. Nothing
aspirational is listed here — a tree that names files which were never written sends the next
reader hunting for them.

```
main/
  main.c                 nvs + config sanity check, listen task, wifi/mqtt start
  listen.c/.h            owns UART1: demux drain loop, decode, publish, bus report
  Kconfig.projbuild      build-time settings (SSID, broker, cell count, baud, expiry)
components/
  jkbms/                 all host-testable, no ESP-IDF dependencies
    jk55.c/.h            55AA framer + DYNAMIC and SETTINGS decoders
    jk_req.c/.h          the BMS's own frame-request writes (closed set, 2 registers)
    jk_demux.c/.h        which of the two is at the front of the stream
    jk_crc.c/.h          CRC-16/MODBUS, used by jk_req and the 308-byte frame variant
    jk_busdiag.c/.h      bus verdict: SILENT / NOISE / UNFRAMED / OVERRUN / NO_DATA / OK
    jk_fmt.c/.h          console rows, hex dump, state and settings JSON
  util/
    strbuf.h             bounded string building, header-only
  net/
    wifi.c               esp_wifi + esp_netif, timer-based reconnect backoff
    mqtt.c               esp-mqtt client, LWT, connect epoch
    ha_discovery.c       retained discovery config publication
test/
  run.sh                 host tests, ASan + UBSan, 226 checks
  test_util.h            check macros + the shared stream-reassembly harness
  host_test_req.c        CRC vectors, frame-request recogniser, 200k-buffer noise test
  host_test_jk55.c       55AA framing, both decoders, stream reassembly
  host_test_util.c       strbuf, demux, bus diagnosis, formatting
```

**Deleted, not deferred.** `mbrtu/` (`mbrtu.c`, `mb_frame.c`), `jk_regs.h`, `jk_decode.c`,
`sniff.c`, `host_test_decode.c` and `host_test_frame.c` were all removed when Track A closed
Phase 1. `bms_snapshot_t` never existed; `jk55_dynamic_t` and `jk55_settings_t` do the job.
There is no sniffer build and no `CONFIG_JK_SNIFFER_MODE` — the normal build is listen-only.

**Not built (deferred to Phase 5):**

- `store/config.c` and `store/provision.c` — configuration is compile-time Kconfig today.
  Nothing reads or writes NVS except the flash init in `app_main`.

**Deliberate simplifications, not omissions:**

- **One task, no mutex.** The sketch further down describes a snapshot filled "under a mutex"
  and a `BMS_UPDATED` event consumed by a separate `mqtt_task`. There is one task; it decodes
  and publishes inline. The decoded structs are never touched by two contexts, so a mutex
  would guard nothing. Add both the moment `mqtt_task` is split out — not before.
- **No `esp_event` loop of our own.** Wi-Fi and MQTT use their own handlers.
- **No poll scheduler.** There is nothing to schedule: the BMS sets the cadence and we read
  whatever it sends.

**One protocol lives here.** `jk55` speaks JK's 55AA push stream, which the BMS emits on
RS485-P with the DIP switches at 0000. `jk_req` recognises the register writes the BMS issues
to provoke those frames — that is traffic we *observe*, not traffic we generate. See
"Protocol decision" above.

Everything under `components/jkbms/` and `components/util/` deliberately has no ESP-IDF
dependencies, so it compiles and is unit-tested on the Mac. That is why `jk_fmt.c` exists as
its own file: the string builders index arrays with values taken straight off the wire, and
they used to live inside the listener where they could not be compiled on the host.

### Tasks — as built

| Task | Prio | Job |
|---|---|---|
| `jklisten` | 5 | Owns UART1. Demux drain loop, decode, JSON, publish, bus report. 8 KB stack. |
| esp-mqtt internal | — | Runs our MQTT event handler, including HA discovery on connect. |
| (default event loop) | — | Wi-Fi/IP state machine via `esp_event` handlers. |

The sketch below was a two-task design (`bms_task` producing, `mqtt_task` consuming a
`BMS_UPDATED` event). It was not built, and the single-task version is not a shortcut: with a
6.4 s cadence and a 576-byte payload there is nothing to pipeline, and the second task would
need the mutex and event plumbing that the one task makes unnecessary.

**Still to do (Phase 5):** subscribe `jklisten` to the TWDT. A BMS comms failure must **not**
reboot the board — it marks entities unavailable and keeps Wi-Fi up so you can see *that* it
failed. Only a prolonged Wi-Fi/MQTT dead state (say 15 min after backoff exhaustion) triggers
a restart.

### Snapshot struct — not built as sketched

> **Superseded 2026-08-31.** There is no `bms_snapshot_t`. The 55AA decoders write
> `jk55_dynamic_t` and `jk55_settings_t` directly (`jk55.h`), which between them carry more
> than this sketch did — the cell-presence bitmap, per-cell wire resistances, SOH, cycle
> capacity, runtime, and the whole SETTINGS frame. Three fields sketched here were never
> available on this unit and are not decoded: `t3_dc`/`t4_dc` (the PB2A16S20P reports two
> sensors plus MOS) and `fw_version` (not in either frame). `comms_ok` was decided against —
> see `jk_fmt.h`. The struct below is kept only to show what the design expected.

```c
typedef struct {
    uint32_t pack_mv;
    int32_t  current_ma;
    int32_t  power_mw;          // derived
    uint8_t  soc_pct;
    uint32_t remaining_mah, full_mah;
    uint32_t cycles; uint32_t cycle_mah;
    uint16_t cell_mv[32]; uint8_t cell_count;
    uint16_t cell_min_mv, cell_max_mv, cell_avg_mv, cell_delta_mv;
    uint8_t  cell_min_idx, cell_max_idx;
    int32_t  balance_current_ma; bool balancing;
    int16_t  mos_temp_dc, t1_dc, t2_dc, t3_dc, t4_dc;
    bool     chg_mos, dsg_mos, balancer_en;
    uint32_t alarms;            // bitfield, decoded to named binary sensors
    int64_t  last_update_us; bool comms_ok;
    char     fw_version[16];
} bms_snapshot_t;
```

---

## 4. MQTT and Home Assistant

Base topic: `jkbms/<device_id>/` where `<device_id>` is derived from the MAC.

**As built, 2026-08-31.** `README.md` is the authority on this section; the summary here is
kept in step with it.

| Topic | Retain | Payload |
|---|---|---|
| `.../status` | yes | `online` / `offline` — firmware alive (LWT) |
| `.../state` | no | full JSON snapshot, **one per DYNAMIC frame (~6.4 s)** |
| `.../settings` | yes | pack configuration, published on change and on reconnect |
| `.../bms_status` | yes | `online` / `offline` — DYNAMIC frames arriving |
| `homeassistant/sensor/<id>_<key>/config` | yes | discovery, published once per MQTT connect |
| `homeassistant/binary_sensor/<id>_<key>/config` | yes | ditto |

**Two JSON payloads, many entities.** Most discovery configs point at `.../state` with a
`value_template` selecting their field; the configuration entities point at `.../settings`
instead. One message per scan cycle regardless of entity count, and HA entities stay in
lockstep.

**Cadence correction.** This section previously said 1 Hz. It is **~6.4 s**, set by the BMS's
scan cycle, and every timeout downstream is sized against that — not against a rate we chose.

**Availability is two-layer, not one.** State entities require *both* `status` and
`bms_status` (`avty_mode: all`), so a dead RS485 link shows as unavailable rather than as
stale-but-plausible numbers. They also carry `expire_after` (`CONFIG_JK_EXPIRE_S`, 30 s),
which is the only thing that catches a publisher wedged mid-loop. Settings entities carry
**neither** — they republish only on change, so expiry would blank a perfectly healthy pack.

Entities as actually published: voltage, current, power, SOC, SOH, cell min/max/avg/delta
(+ min/max index), cell count, MOSFET temp and T1/T2, balance current, remaining/full
capacity, cycles, total cycled, BMS runtime, alarm bits, worst wire resistance, RSSI, uptime,
frame count; charge/discharge/balance MOSFET and BMS comms as binary sensors; ten
configuration sensors and three configuration switches read from the SETTINGS frame; and
`CONFIG_JK_CELL_COUNT` per-cell voltages with `enabled_by_default: false`.

Two things the sketch above expected that are **not** built: **T3/T4** (this unit reports two
sensors plus MOS) and **alarm bits decoded to individual `device_class: problem` binary
sensors** (the raw bitfield is published as one diagnostic sensor; splitting it needs a bit-to-
meaning map this firmware has never seen documented). "Poll error count" and "BMS firmware
version" are gone with the poller — the bus-health verdict replaces the former.

---

## 5. Phased build

| Phase | Deliverable | Done when |
|---|---|---|
| **0** | Board bring-up | Blink runs; `flash_id` recorded; USB-RS485 dongle sees bytes from GPIO17 with DE toggling correctly |
| **1** | First real read | ✅ **Done 2026-08-31.** 55AA frames decode; five independent cross-checks agree with the JK app. Slave address, baud and framing all resolved — and the register-map question is moot, since nothing reads `jk_regs.h` any more. |
| **2** | Full decode | ✅ **Done 2026-08-31.** Both frame types decode into `jk55_dynamic_t` / `jk55_settings_t` and print to console each cycle; cell voltages match the app. |
| **3** | Networking | ✅ **Code complete 2026-08-31**, pending on-hardware sign-off. Wi-Fi + MQTT connect, state JSON published, LWT configured. Config is compile-time Kconfig, **not** NVS — NVS moved to Phase 5. |
| **4** | Home Assistant | ✅ **Code complete 2026-08-31**, pending on-hardware sign-off. Discovery configs published on every connect; two-layer availability with `expire_after`. **Cable-pull test not yet run** — that is the outstanding acceptance criterion. |
| **5** | Hardening | NVS config + SoftAP provisioning page, OTA with rollback, TWDT, retry/stale-data behaviour verified by pulling the cable |
| **6** | Optional | CAN output to inverter, or a local web dashboard — both already have pins/headroom |

~~Phase 1 is where the schedule risk lives.~~ **The risk did not materialise.** Phase 1 closed
on its first attempt by taking the 55AA path, which needed no protocol archaeology at all —
the field offsets were documented, and a listen-only build confirmed them in eleven seconds.
Everything from Phase 2 on has been the routine work it was always expected to be — Phases 2
through 4 were written in the same session that closed Phase 1. The advice this paragraph used
to end with, to spend a session with a USB-RS485 dongle confirming the register layout, is
obsolete: there is no register layout to confirm.

### Test strategy

- Everything under `components/jkbms/` and `components/util/` compiles for the host with no
  ESP-IDF dependency. `sh test/run.sh` builds and runs **226 checks** under ASan + UBSan with
  `-Wall -Wextra -Werror -Wconversion -Wshadow` in about a second. Set `NOSAN=1` to skip the
  sanitizers.
- Tests build synthetic frames from the documented offsets, so they verify the decoder against
  the specification. They cannot verify the specification against hardware — the five
  cross-checks in §2.3 are what does that, and they are recorded in `jk55.h`.
- The frame-request recogniser is fed 200,000 random buffers and must never accept one. This
  is a regression test for the deleted RTU framer, which fabricated a "valid" frame roughly
  every 588 bytes of noise.
- ~~A small Python Modbus slave (pymodbus) replays a captured register image.~~ Not needed and
  not written: the firmware runs fine with nothing on the RS485 terminals, reporting `SILENT`,
  so Phases 3–5 develop with no battery attached anyway.
- **Cable-pull test — still outstanding.** Yank RS485, confirm HA goes unavailable (not
  stale-but-plausible), reconnect, confirm recovery without a reboot. This is an explicit
  acceptance criterion for Phase 4 and has not yet been run.

---

## 6. Deliberately excluded

**Writes to the BMS.** Function `0x10` can flip charge/discharge MOSFETs and change protection
setpoints. A bug in a loop that occasionally writes is a fire-adjacent bug, so the firmware has
no write path at all — not a disabled one. Taking Track A made this **structural rather than a
policy**: the UART is opened with no TX pin and DE is held low as a plain GPIO, so the
firmware physically cannot put a byte on the wire. If you want writes later, the shape is: a
compile-time flag, a separate `.../cmd` topic, an allow-list of writable registers, and a
read-back-and-verify after every write — and you would have to route a TX pin first, which is
exactly the deliberate speed bump.

**Inverter emulation over CAN.** The pins and driver are free, but pretending to be a
Pylontech pack is a project of its own.

---

## Open items

**Updated 2026-08-31.** Items 2 and 3 were closed by Track A: there is no protocol `001` to
set (the DIP stays at `0000`) and no register table to validate.

1. ~~Confirm module flash/PSRAM~~ — **done**, 16MB flash / 8MB PSRAM, measured with
   `esptool.py flash_id`. Still open: whether USB-C 5V powers the isolated RS485 rail.
2. ~~Confirm which BMS port is free and set its protocol to `001`.~~ Moot — RS485-P at DIP
   `0000`, no protocol setting involved.
3. ~~Validate the register table against the vendor PDF and a live capture.~~ Moot — no
   register table.
4. Decide MQTT broker address / credentials, and whether HA is on the same VLAN as the ESP32
   (this box will want a static lease and probably belongs on the IoT segment). **Still open** —
   the build still ships `SET_ME` placeholders.
5. **Run the cable-pull test.** The one Phase 4 acceptance criterion not yet exercised.

## Sources

- Waveshare board GPIO map: `Sleeper85/esphome-yambms` → `documents/README/Board_Waveshare_ESP32-S3-RS485-CAN.md`
- JK Modbus register details: `phinix-org/Multiple-JK-BMS-by-Modbus-RS485`
- Vendor protocol PDF: `syssi/esphome-jk-bms` → `docs/pb2a16s20p/BMS RS485 Modbus V1.1.pdf`
- V19 specifics and bugs: `syssi/esphome-jk-bms` discussion #747; `jean-luc1203/jkbms-rs485-addon` discussion #59
- **Vendor docs for this exact model:** `syssi/esphome-jk-bms` -> `docs/pb2a16s20p/`, plus
  "RS485 Communication example.pdf". Kept as a cross-reference for frame offsets; the
  `jk_regs.h` they were meant to validate no longer exists.
- **55AA push protocol, field-by-field — this is the source we actually implemented from:**
  `fancyui/Gobel-Battery-HA-Integration` -> `JK-BMS-55AA-Protocol_EN.md`. Written for
  PB1A16S10P. ~~Unverified on PB2A16S20P.~~ **Verified 2026-08-31** by five independent
  cross-checks against the JK app; see §2.3 and the header comment in `jk55.h`. Two of its
  claims did *not* hold on this unit: the 308-byte CRC16 frame (ours are 300-byte sum8) and
  the 24 extra tail bytes on SETTINGS frames (ours have none).
- **Closest prior art on this model:** `jean-luc1203/jkbms-rs485-addon` — Modbus RTU over
  RS485, lists PB2A16S20P for firmware 14/15/19, supports passive broadcast listening.
- RJ45 pinouts: jkbms.net → solutions/inverter-bms-communication
