# JK BMS monitor — build checklist, all phases

Working companion to `jk-bms-esp32s3-firmware-plan.md`. The plan says *what* and *why*; this
says *do this, then this*, in order, with a box to tick.

**Target:** Waveshare ESP32-S3-RS485-CAN → one JK inverter BMS (hw V19) over RS485 Modbus →
MQTT → Home Assistant.

**Tested on:** an Apple Silicon (arm64) Mac with zsh, Homebrew and Python 3.13.

---

## Status as of 2026-08-31

| Phase | State |
|---|---|
| 0 — Toolchain | **Done, verified.** ESP-IDF builds this project on this machine. |
| 0 — Board bring-up | **Done, verified.** Flashed and running own firmware. |
| 1 — First real read | **Track A SUCCEEDED 2026-08-31.** 55AA frames decoding, values verified. |
| 2 — Full decode | **Done, verified on hardware.** DYNAMIC and SETTINGS both decode; console output matches the JK app. |
| 3 — Networking | **Code complete, not yet signed off on hardware.** Wi-Fi, MQTT, state JSON and LWT are all written and build clean. Not verified against a live broker. |
| 4 — Home Assistant | **Code complete, not yet signed off on hardware.** Discovery, two-layer availability and `expire_after` are written. **Cable-pull test not run.** |
| 5–6 | Not started. |

**Where you actually are:** Phases 0–2 are closed and verified. Phases 3 and 4 are *written*
but have never been pointed at a real broker or a real Home Assistant — treat every unticked
box below as genuinely unticked, because the code building is not the same as the entity
appearing on a dashboard.

Track B — the Modbus register archaeology that was the project's largest scheduled risk — is
gone, along with the code that implemented it. There is no `bms_snapshot_t`, no `jk_regs.h`,
no `mbrtu`, and no separate sniffer build; the normal build is listen-only.

**Next step:** flash it, point it at the broker, and work down the Phase 3 and Phase 4
checkboxes. The first thing that will stop you is the `SET_ME` placeholders — the firmware
now names each unset one at boot.

### Confirmed on the wire, 2026-08-31

| Finding | Value |
|---|---|
| Frame format | **300 bytes, additive `sum8`** — not the 308-byte CRC16 variant |
| The "308-byte frame" in the docs | really 300-byte frame **+ a separate 8-byte Modbus 0x10 echo** |
| Bus topology | **the BMS masters its own bus at DIP `0000`** and answers as pack 0 |
| Scan pattern | writes `0x1620` (dynamic) / `0x161E` (settings) to addresses `00`–`0F`, 200 ms apart |
| Full cycle | ~6.4 s, so one DYNAMIC frame every ~6.4 s |
| Our role | **pure listener — we never transmit, and never need to** |

The BMS generating its own poll traffic is why no request is needed from us: at DIP `0000` it
scans for parallel packs, finds itself at address 0, and answers. Addresses `01`–`0F` get no
reply because there are no other packs.

**First decoded frame, cross-checked:**

| Check | Result |
|---|---|
| Sum of 16 cells vs reported pack voltage | 52390 mV vs 52388 mV — 2 mV |
| V × I vs reported power | 84.6 W vs 84.606 W |
| remaining/full vs reported SOC | 61.9% vs 62% |
| runtime vs JK app "Total Time" | 73.16 days vs 72D15H20M — matches |
| computed cell delta vs reported | 2 mV vs 2 mV |

Five independent agreements. The offsets in `jk55.c` are validated for this firmware.

Verified toolchain versions (not aspirational — these were run):

| | |
|---|---|
| ESP-IDF | `v5.5.5-585-gfd0b33dfda0`, branch `release/v5.5` |
| Compiler | `xtensa-esp32s3-elf-gcc` 14.2.0 |
| Python env | `idf5.5_py3.13_env` |
| Poller build | `0xdee50` (891 KB), 41% of the app partition free |
| Sniffer build | `0x38ab0` (227 KB), 85% free |
| Board | ESP32-S3 QFN56 rev v0.2, 16MB flash, 8MB PSRAM, `/dev/cu.usbmodem1101` |

> **The Python 3.12 workaround is not needed on this machine.** The old version of this doc
> warned that `install.sh` might reject the python.org 3.13 build. It didn't — the env is
> `idf5.5_py3.13_env` and the build works. Ignore any advice to install `python@3.12`.

---

# Phase 0 — Toolchain and board bring-up

**Done when:** you have flashed firmware you compiled yourself and watched it print over
serial, and you have recorded the module's flash size.

## Part A — Toolchain ✅ complete

Kept for reference / rebuilding on another machine.

```bash
brew install cmake ninja ccache
```

```bash
mkdir -p ~/esp && git clone -b release/v5.5 --recursive https://github.com/espressif/esp-idf.git ~/esp/esp-idf
```

```bash
~/esp/esp-idf/install.sh esp32s3
```

```bash
echo "alias get_idf='. \$HOME/esp/esp-idf/export.sh'" >> ~/.zshrc
```

The leading `.` inside the alias matters — the script must be **sourced**, not executed.
Running `./export.sh` appears to work and then silently does nothing.

- [x] `cmake`, `ninja`, `ccache` installed
- [x] ESP-IDF cloned (4.0 GB, submodules complete)
- [x] `install.sh esp32s3` finished; `~/.espressif/tools` has all six toolchain dirs
- [x] `get_idf` alias in `~/.zshrc:12`
- [x] `idf.py --version` prints `v5.5.5-585-gfd0b33dfda0`

**Every new terminal needs `get_idf` before any `idf.py` command.** This is the single most
common "it broke" cause; see Troubleshooting.

## Part B — Connect the board ✅ complete

### B1. Plug in

USB-C to the Mac. macOS may prompt "Allow accessory to connect?" — allow.

### B2. Find the serial port

```bash
ls /dev/cu.*
```

Baseline with nothing plugged in (confirmed on this Mac): `cu.Bluetooth-Incoming-Port`,
`cu.debug-console`, `cu.wlan-debug`. The **new** entry is the board.

| What you see | Meaning |
|---|---|
| `/dev/cu.usbmodem____` | ESP32-S3 native USB. Ideal — no driver needed. |
| `/dev/cu.wchusbserial____` / `/dev/cu.usbserial____` | USB-serial bridge chip. Fine; macOS 26 has the driver. |

**If nothing new appears:** charge-only USB-C cable is the usual culprit. Try a known-good
data cable before suspecting anything else.

- [x] Port name: **`/dev/cu.usbmodem1101`** — native USB, no driver needed.
      (Called `$PORT` below. Note it can change across reboots or USB ports.)

### B3. Confirm the chip responds

```bash
esptool.py -p $PORT flash_id
```

Reports chip type, MAC, flash manufacturer and **flash size** — the open item in the plan.

**If it won't connect:** hold **BOOT**, tap **RESET**, release BOOT, re-run.

- [x] **ESP32-S3 (QFN56) rev v0.2, 16MB flash, 8MB embedded PSRAM**, quad SPI, 3.3V,
      USB-Serial/JTAG. MAC `xx:xx:xx:aa:bb:cc`.

Two things follow from this:

- `sdkconfig.defaults` said 4MB as a safe guess. Corrected to **16MB** — it matters, because
  Phase 5's OTA-with-rollback needs two app partitions and a 4MB declaration would have capped
  the partition table before that was possible.
- Your MQTT device id is derived from the last three MAC bytes, so every topic will be
  **`jkbms/jkbms-aabbcc/...`**.

**Note the exact syntax** — two easy trips:

```bash
esptool.py -p /dev/cu.usbmodem1101 flash_id
```

The port needs `-p` (bare paths are read as a subcommand), and esptool **v4.12** — the version
ESP-IDF 5.5 ships — spells subcommands with underscores (`flash_id`). esptool v5 switched to
hyphens (`flash-id`), which is what most current web results show. If a command errors with
`invalid choice`, that mismatch is why.

## Part C — First flash ✅ complete

The project itself already compiles on this machine, so the `hello_world` example is optional.
Flashing the real firmware proves the same chain (compile → flash → run → serial back).

```bash
get_idf
```

```bash
idf.py -p $PORT flash monitor     # from the repo root
```

With no BMS attached this is the expected, *correct* output: Wi-Fi fails to associate (the
credentials are still `SET_ME`), and the listener reports a silent bus every 10 s. That is the
board running your code.

**Exit the monitor with `Ctrl` + `]`** — not Ctrl-C, which can leave the port locked.

- [x] **Firmware I compiled is running on the board.** (2026-08-31)

What the **current** build prints with nothing wired and `SET_ME` credentials:

```
E jkmon: Wi-Fi SSID is still the placeholder ("SET_ME")
E jkmon: Wi-Fi password is still the placeholder
E jkmon: MQTT broker URI is still the placeholder ("mqtt://SET_ME:1883")
E jkmon: these are BUILD-TIME settings: run 'idf.py menuconfig' ...
I jk: listening: uart1 rx=18 de=21(low) 115200 baud, tx disabled
I jk: 10s: 0 bytes, 0 req, 0 55AA, 0 dropped | errs: 0 frame, ...
W jk:   -> nothing on the pair. Check: DIP at 0000 ...
W wifi:Haven't to connect to a suitable AP now!
```

`Haven't to connect to a suitable AP now!` is esp_wifi's own phrasing for "SSID not found".

> **Historical, 2026-08-31 — this was the *poller* build, which no longer exists.** The first
> flash produced `W mbrtu: reg 0x0080 failed after 3 attempts` and `W jkmon: poll failed
> (ESP_ERR_TIMEOUT), consecutive=23`, with poll cycles measured **2360 ms** apart against a
> predicted 2300 ms (3 × 300 ms timeout + 2 × 200 ms retry gaps + 1000 ms poll interval),
> confirming the retry and t3.5 timing arithmetic. Recorded because that measurement is the
> only on-hardware evidence the Modbus timing analysis was ever right. **You will not see any
> of it now** — there is no poll loop and no `mbrtu`.

---

# How to reach the config menu

You'll need this from Phase 1 on. Two routes.

### Route 1 — the menuconfig TUI

```bash
idf.py menuconfig
```

This is a **full-screen interactive terminal program**. Run it in your own Terminal window; it
needs a real TTY, so it won't work through a non-interactive shell.

| Key | Does |
|---|---|
| `↑` `↓` | Move between items |
| `Enter` | Enter a submenu |
| `Space` / `Y` / `N` | Toggle a `[ ]` bool on/off |
| `Esc` `Esc` | Back up one level |
| `/` | **Search.** Type e.g. `JK_BAUD`, then the number next to a hit to jump to it. |
| `S` | Save |
| `Q` | Quit (prompts to save) |

All this project's settings live under one top-level entry: **`JK BMS Monitor Configuration`**.
Inside it, and this is the complete list (`main/Kconfig.projbuild`): Wi-Fi SSID/password, MQTT
broker URI/username/password, cell count, RS485 baud, and the Home Assistant `expire_after`.

The sniffer-mode, slave-address, poll-interval and u32-word-swap settings this section used to
list are **gone**, deleted with the Modbus poller.

The `/` search is much faster than hunting through the menu tree — most of what you see at the
top level is ESP-IDF's own thousand-option config, not yours.

### Route 2 — edit `sdkconfig` directly (no TUI)

Better when you just want one value changed, and it's scriptable. `sdkconfig` is a plain text
file: `overlay/sdkconfig` when you keep credentials in the overlay (see the README), otherwise
`sdkconfig` in the project root. For example, set the cell count:

```bash
sed -i '' 's/^CONFIG_JK_CELL_COUNT=.*/CONFIG_JK_CELL_COUNT=16/' overlay/sdkconfig
```

The next `idf.py build` picks it up, and the edit preserves everything else in the file
(Wi-Fi credentials included).

> The examples here used to toggle `CONFIG_JK_SNIFFER_MODE`. That option no longer exists —
> there is one build and it is listen-only.

Note the odd-looking `-i ''` — that's BSD `sed` on macOS requiring an explicit empty backup
suffix. GNU `sed -i` syntax from Linux tutorials will fail here.

> **A Kconfig `bool` that is off is not defined as `0` — it is absent from `sdkconfig.h`
> entirely.** So `#if CONFIG_FOO` is fine (undefined → 0), but using `CONFIG_FOO` in C code is
> a compile error when it's off. This already bit us once: `CONFIG_JK_U32_WORDSWAP` was used
> directly in `main.c`, so the project failed to build in its default configuration. Fixed with
> an `#ifdef` wrapper — use that pattern for any new bool.

---

# Phase 1 — First real read

**The risky phase.** Everything after it is routine. This is protocol archaeology against a
firmware version whose RS485 support is still marked incomplete upstream.

**Done when** *either* track below produces battery data that matches the JK app:

| Track | Exit criterion |
|---|---|
| **A — 55AA push** | ✅ `55 AA EB 90` frames decode; pack voltage, SOC and cells match the app |
| **B — Modbus poll** | ~~a `0x03` read decodes to values matching the app~~ — never run, code deleted |

Track A was the one to try first, and it ended the phase outright — it needed no register map,
no slave address, and no transmission. Track B was the fallback and where the schedule risk
lived; it was never needed.

### Your unit (from the JK app, 2026-08-30)

| | |
|---|---|
| Vendor ID | **JK-PB2A16S20P** |
| Hardware / software | V19A / **V19.31** |
| Hardware option | CEHMPRT |

**V19.31 means the DIP protocol bug in 1b definitely applies to you** — it affects firmware
≥ V19.10. Re-check the protocol field after every power cycle, not just the first time.

Your exact model has vendor documentation upstream: `syssi/esphome-jk-bms` →
`docs/pb2a16s20p/`, plus "RS485 Communication example.pdf". Worth keeping as a reference for
the *frame* offsets, though the 55AA offsets we actually use came from
`fancyui/Gobel-Battery-HA-Integration` (`JK-BMS-55AA-Protocol_EN.md`) and are recorded in
`jk55.h`. (Its ESPHome *code* won't help — that component is UART-TTL and BLE only, with no
RS485 Modbus for PB-series. The docs folder is the prize.)

`jean-luc1203/jkbms-rs485-addon` is the closest working prior art: Modbus RTU over RS485,
PB2A16S20P listed explicitly for firmware 14/15/19, and it supports passive broadcast
listening.

### 1a. Which socket — settled by the vendor spec

The [JK-PB2A16S20P BMS Specification V1.0](https://www.gobelpower.com/download/JKBMS-JK-PB2A16S20P-Specication-EN-V1.0.pdf) (JK, 2024-03-08) is authoritative for this unit.

| Port | Connector | Purpose |
|---|---|---|
| CAN | RJ45, paired with RS485-1 in one dual block | inverter, 250k |
| RS485-1 | RJ45, the other half of that block | inverter comms |
| **RS485-2** | **two RJ45s wired in parallel** | **battery monitoring — use this** |
| RS232 | RJ11 6P6C (smaller) | firmware upgrade, 9600 |

> Spec section 8.3: "There are two RS485 communication interfaces, one is a parallel output
> with two ports for viewing battery pack information. Default baud rate 115200."

RS485-2 is the monitoring interface, it runs at 115200 (matching `CONFIG_JK_BAUD`), and using
it leaves RS485-1 free for the inverter. The Gobel doc's "RS485B / RS485C" are these two
sockets.

**Pin assignments — identical on RS485-1 and RS485-2:**

| Pin | Signal |
|---|---|
| 1, 8 | RS485-B (−) |
| 2, 7 | RS485-A (+) |
| 3, 6 | GND |
| 4, 5 | NC |

**CAN, for contrast:** pin 4 CANL, pin 5 CANH, pin 7 GND, and **pins 1, 2, 3, 6, 8 all NC**.
A jack that is dead on pins 1 and 2 is CAN, not RS485.

**Identified visually from the board photo (2026-08-31) — no meter needed.** Board rev V1.02,
part `78160G.P365_251028`. Silkscreen, left to right:

| Position | Silkscreen | What it is |
|---|---|---|
| Dual RJ45 (J2), left jack | `RS485` | RS485-1, inverter |
| Dual RJ45 (J2), right jack | `CAN` | CAN |
| Middle 6-pin | — | RS232 (RJ11) |
| Dual RJ45 (J1), both jacks | **`RS485-P`** | **RS485-2 parallel pair — use either** |

"RS485-**P**" is P for Parallel: the spec's "parallel output with two ports for viewing battery
pack information". The pin table printed on the board confirms the spec exactly:

| 01 | 02 | 03 | 04 | 05 | 06 | 07 | 08 |
|---|---|---|---|---|---|---|---|
| 485B | 485A | GND | NC | NC | GND | 485A | 485B |

(The CAN table on the board reads `04 = CANH, 05 = CANL, 02 = GND`, which differs slightly
from the PDF's numbering. Irrelevant here, but trust the silkscreen over the PDF if CAN is
ever used.)

**Possible shortcut — skip crimping.** There is a 6-pin header beside RS485-P, silkscreened
`GND0 GND0 TX RX 485-A 485-B`. If those 485-A/485-B pins tap the same net as the RS485-P
jacks, two Dupont jumpers replace the whole RJ45 crimp for bench work. **Verify first**, BMS
powered off: continuity from header `485-A` to **pin 2** of an RS485-P jack.

- [x] Socket identified: **RS485-P** (either jack of the J1 pair)
- [ ] Header shortcut checked: does `485-A` ring out to RS485-P pin 2? ______

> **Still to locate: the 4-bit DIP switch.** It is not visible in the port-area photo. You
> need it to choose Track A (`0000`) vs Track B (address 1-15) — it is the setting that decides
> which protocol the BMS speaks. Find it before wiring.

### 1b. BMS-side configuration

**Do this after you have picked the socket, and before wiring anything.** Which settings you
want depends on which track you are running — see 1d and 1e. They are mutually exclusive.

**DIP address, 4-bit, switch 1 = LSB** (spec section 8.4):

| Address | 1 | 2 | 3 | 4 |
|---|---|---|---|---|
| 0 | OFF | OFF | OFF | OFF |
| 1 | ON | OFF | OFF | OFF |
| 2 | OFF | ON | OFF | OFF |
| 3 | ON | ON | OFF | OFF |
| 15 | ON | ON | ON | ON |

**Address 0 and addresses 1–15 select different behaviours, and both are useful:**

| DIP | BMS behaves as | Track | Build |
|---|---|---|---|
| `0000` (addr 0) | transmits on its own — the 55AA push stream | **A** | sniffer |
| addr 1–15 | Modbus slave, answers when polled | **B** | poller |

This reconciles two pieces of advice that looked contradictory: address 0 "fights your poller"
*because* it pushes instead of answering — which is exactly what a listen-only build wants.

**Protocol field (Track B only).** Set the port's protocol to `001 - JK BMS RS485 Modbus V1.0`
(or `013` for 9600 baud). Leave it alone for Track A.

> **Known V19 bug, and you are affected.** On firmware ≥ V19.10 the DIP address does not
> reliably auto-configure the port protocol. You are on **V19.31**. After setting the DIP,
> re-open the app and confirm the protocol field actually reads what you expect — and
> **re-check after every BMS power cycle**, not just the first time.

- [ ] DIP set to: `____`  (track A = 0000, track B = 1–15)
- [ ] Protocol field verified in the app after setting it: `________`

### 1c. Wiring

RS485-1 is an RJ45; RS485-2 is a 4-pin connector. Use whichever the inverter is *not* using.

| Signal | RS485-1 (RJ45) | RS485-2 (4-pin) | Board terminal |
|---|---|---|---|
| A (+) | pins 2 and 7 | pin 2 | A |
| B (−) | pins 1 and 8 | pin 1 | B |
| GND | pins 3 and 6 | pin 3 | GND |

- One twisted pair for A/B, plus a third conductor for GND. **Signal ground is not optional** —
  the isolated transceiver needs a reference.
- Enable the board's **120Ω** termination jumper. With two nodes the ESP32 is a bus end.
- Keep RS485 GND bonded **only** to the BMS. Do not tie it to the auxiliary supply's ground —
  that isolation is what keeps a 48V-referenced BMS ground out of your bench supply.

### Building the cable (Cat6a, T568B)

RJ45 pin 1 is on the **left** with the contacts facing you and the latch underneath.

| RJ45 pin | T568B colour | Signal |
|---|---|---|
| 1 | white/orange | **B (−)** |
| 2 | orange | **A (+)** |
| 3 | white/green | not landed — this board has no RS485 GND terminal |
| 6 | green | not landed |
| 7 | white/brown | A (+) — parallel duplicate, optional |
| 8 | brown | B (−) — parallel duplicate, optional |
| 4, 5 | blue pair | unused |

A/B on the **orange pair** (pins 1-2) puts the differential signal on one twisted pair, which
is the point of using twisted pair at all.

**This board's RS485 terminal is A+ / B- only — there is no ground pin, by design.** The
interface is galvanically isolated, so the field side floats and self-references to the bus.
Two wires is correct here, not a compromise. Crimp the full RJ45 so the plug seats, but land
only the orange pair.

**Never improvise a ground** onto the 7-36V power terminal GND or a header pin: that bridges
the isolation barrier and bonds battery negative to the board and, over USB, to the laptop.

Solid-core is fine for screw terminals — better than stranded. Do not tin the ends.

**Sanity-check before landing wires.** The vendor spec (section 1a) gives the same pinout
for RS485-1 and RS485-2, so this is confirmation rather than discovery — but a two-minute
check beats a dead transceiver. With the BMS powered and nothing connected, measure each pin
against battery negative:

| Reading | Meaning |
|---|---|
| ~2.5 V on two pins, small differential between them | A / B |
| 0 V | GND |
| Anything near pack voltage | **Stop.** Do not land it on the transceiver. |

That last row is why this is a measurement and not an assumption — a pack-voltage pin into an
A/B input destroys the transceiver instantly. Also note the CAN RJ45 is physically identical
to the RS485 ones; wrong port means silence, which is easy to misread as a protocol problem.

**Termination:** with only the BMS and this board on the bus you are one of two ends, so the
120R jumper is textbook-correct — though at 115200 over a short lead it will likely work
either way. Leave it **off** if you are tapping a bus the inverter already uses: both ends are
already terminated there. Symptom that points at termination is frames arriving with failing
checksums, not an absence of frames.

- [ ] A/B wired (orange pair), pinout verified with a meter, isolation left intact
- [ ] Termination jumper decided based on topology

### 1d. Track A — 55AA push stream (try this first)

**Why first:** no register map, no slave address, and the firmware never transmits. If frames
appear, the Modbus map-validation exercise the plan budgeted sessions for becomes optional.
(It did, and it is.)

Set **DIP `0000`**, wire to the **RS485-P** socket, and flash. There is no sniffer flag to
set — the only build there is is listen-only:

```bash
idf.py -p /dev/cu.usbmodem1101 flash monitor
```

A decoded 55AA frame looks like this. Real output from this unit:

```
I jk: DYNAMIC len=300 (sum8)  52388 mV  -1615 mA  84606 mW  soc 62% soh 100%
I jk:      cells=16 present=0x0000FFFF avg=3274mV delta=2mV max=#4 min=#1
I jk:      mos=28.6C t1=26.2C t2=26.4C bal=0mA alarms=0x00000000 chg=1 dsg=1 bal=0
I jk:      remain=194320mAh full=314000mAh cycles=11 cycle_cap=3527140mAh runtime=6321223s
        cell 01-08: 3273 3274 3274 3275 3275 3275 3275 3274 mV
        cell 09-16: 3274 3275 3275 3274 3275 3274 3274 3274 mV
```

It prints pack voltage, current, power, SOC, SOH, all cells, the cell-presence bitmap,
temperatures, balance current, alarms, MOS states, capacity, cycles and runtime. SETTINGS
frames are logged once and then only when something actually changes.

**Which integrity check passes is itself a finding.** The source document describes a 308-byte
frame with a trailing Modbus CRC16; the widely-deployed JK02 variant uses a 300-byte frame
ending in an additive sum byte. `jk55.c` tries both and names the winner in the log. Record it.

The 10 s bus report names which of these you are in — see the verdict table in
`README.md`, or `jk_bus_advice()` for the one-line fix suggestion:

| What you see | Means |
|---|---|
| `-> link OK` with `DYNAMIC …` lines | Track A works. Skip Track B entirely. |
| `-> nothing on the pair` (`SILENT`) | Nothing is transmitting. Wrong socket, wrong DIP, or a sleeping BMS. |
| `-> line is active but unreadable` (`NOISE`) | A/B swapped, or wrong baud. Swap the pair first, it costs nothing. |
| `-> clean bytes, nothing parses` (`UNFRAMED`) | Baud is right, protocol is not one we decode. |
| `-> bus is healthy … no pack answered` (`NO_DATA`) | The BMS is scanning but nothing replies. |
| `55AA frame byte4=0x.. did not decode` | Framing works, offsets are wrong for your firmware. Capture the hex and adjust `jk55.c`. |

- [x] Firmware flashed, DIP `0000`, RS485-P socket
- [x] `55 AA EB 90` frames seen? **Yes, ~6.4 s apart**
- [x] Which check passed? **sum8** (300-byte frames; the 308-byte CRC16 variant never appeared)
- [x] Decoded values vs JK app: **five independent cross-checks agree** (see status block)

> ~~The offsets in `jk55.c` are unverified on V19.31.~~ **Verified 2026-08-31.** Pack voltage,
> power, SOC and runtime decode from four unrelated byte offsets and all four agree with
> independently derived values, so the offsets are right for this firmware — not merely
> self-consistent.

**Cadence matters downstream.** One DYNAMIC frame per ~6.4 s scan cycle, not 1 Hz. Anything
that times out — `expire_after`, staleness, a future watchdog — must be sized against that.
`CONFIG_JK_EXPIRE_S` (default 30 s) covers several missed cycles. The listener measures the
real interval and prints it, so you are never sizing against arithmetic alone.

### 1e. Track B — Modbus poll (fallback) — NOT NEEDED, AND NO LONGER IMPLEMENTED

Track A succeeded, so this was never run — and the code that would have run it has since been
deleted. `mbrtu.c`, `mb_frame.c`, `jk_regs.h`, `jk_decode.c` and the slave-address,
poll-interval and word-swap settings are all gone. The `decode_warn` plausibility flag and the
`JK_ST_COUNT` compile-time bounds check went with them.

**If a firmware update ever stops the BMS self-polling, this track has to be rebuilt from
scratch, not re-enabled.** That is a deliberate trade: keeping an unvalidated, untested Modbus
master around "just in case" meant carrying a register map that was pure hypothesis and an RTU
framer that fabricated valid-looking frames out of noise roughly every 588 bytes — which is
exactly the bug that made a dead bus report healthy. Deleting it was the fix.

The starting points if it ever comes back: set **DIP 1–15** and protocol `001`; the register
hypothesis is preserved in the plan's §2.1; validate against `docs/pb2a16s20p/BMS RS485 Modbus
V1.1.pdf` in `syssi/esphome-jk-bms` before trusting any of it. Note that passive sniffing
shows nothing on this track — being the master is what generates the traffic.

---

# Phase 2 — Full decode ✅ complete

**Done when:** both frame types decode, print to the console each scan cycle, and the cell
voltages match the JK app.

- [x] DYNAMIC frame decodes: voltage, current, power, SOC/SOH, temps, cells, cycles, runtime
- [x] Cell presence bitmap drives min/max/avg/delta and their indices
- [x] SETTINGS frame decodes: OVP/UVP thresholds, current limits, capacity, switches
- [x] Current **sign** verified — a resting discharge reads negative and V × I matches the
      BMS's own reported power to 0.006 W
- [x] Synthetic frames added to the host tests (`test/run.sh`) so decode regressions get
      caught on the Mac, with no board attached

Everything under `components/jkbms/` and `components/util/` deliberately has no ESP-IDF
dependency precisely so this works. `sh test/run.sh` runs **226 checks** under ASan + UBSan in
about a second — run it before every flash; it is far cheaper than discovering a decode bug
through Home Assistant.

> The checkboxes here originally named Modbus register blocks (`0x0080`–`0x00AF`, `0x1200`).
> Those were Track B's exit criteria and no longer mean anything — the 55AA frame carries all
> of it at documented byte offsets instead.

---

# Phase 3 — Networking

**Code complete; none of these boxes verified on hardware yet.**

**Done when:** Wi-Fi connects, MQTT connects, JSON state publishes, LWT works.

- [ ] Real Wi-Fi SSID/password set (they are `SET_ME` placeholders until you change them —
      the firmware names each unset one at boot)
- [ ] MQTT broker URI, username and password set in menuconfig, then **rebuilt and reflashed**
      (they are build-time settings; editing them alone changes nothing)
- [ ] Broker reachable at the address you configured
- [ ] `jkbms/<id>/state` arrives **once per ~6.4 s scan cycle** — not once a second:

```bash
mosquitto_sub -h <broker> -u <user> -P <password> -v -t 'jkbms/jkbms-aabbcc/#'
```

- [ ] `jkbms/<id>/settings` arrives once, retained, shortly after MQTT connects
- [ ] LWT verified: pull board power, `jkbms/<id>/status` flips to `offline`
- [ ] `jkbms/<id>/bms_status` flips to `offline` ~25 s after the RS485 lead is pulled

> **Secrets are compiled in.** Once set, your MQTT password lives in `sdkconfig` (in the
> gitignored `overlay/` when you use one), `build/config/sdkconfig.h` and the flashed `.bin`.
> All of them are gitignored. The binary is sensitive too: do not share a build artifact
> assuming the password is not in it.

This phase can be done **with no battery attached** — the firmware runs fine with nothing on
the RS485 terminals. The 10 s bus report reads `SILENT` and `bms_status` stays `offline`,
while Wi-Fi and MQTT work normally.

---

# Phase 4 — Home Assistant

**Code complete; none of these boxes verified on hardware yet.**

**Done when:** the device card populates and availability flips correctly when you unplug the
RS485 lead.

- [ ] Retained discovery configs land under `homeassistant/sensor/<id>_*/config` and
      `homeassistant/binary_sensor/<id>_*/config`
- [ ] One device card, all entities grouped under it
- [ ] State entities show *unavailable* when `bms_status` is offline — they require **both**
      status topics online (`avty_mode: all`)
- [ ] **Configuration entities populate** (cell count, OVP/UVP, limits, float voltage…) — they
      read the retained `settings` topic, carry no `expire_after`, and must *not* go
      unavailable just because the pack configuration has not changed
- [ ] **"BMS comms" reads `disconnected`**, not merely *unavailable*, when the RS485 lead is
      out. It reads the `bms_status` topic directly for exactly this reason
- [ ] Per-cell voltages present but `enabled_by_default: false`
- [ ] **No Jinja template errors in the HA log.** If the pack reports fewer cells present than
      `CONFIG_JK_CELL_COUNT`, the surplus cell entities should read *unknown*, not raise
- [ ] **Cable-pull test:** yank the RS485 lead → entities go unavailable, *not* stale-but-
      plausible. Reconnect → recovery with no reboot.

That last one is an explicit acceptance criterion, not a nice-to-have. Stale-but-plausible data
on a battery dashboard is worse than no data.

---

# Phase 5 — Hardening

- [ ] Config moved to NVS (Wi-Fi creds, broker, cell count)
- [ ] SoftAP + HTTP provisioning page as a headless fallback
- [ ] OTA with rollback
- [ ] Task watchdog on the `jklisten` task
- [ ] **A BMS comms failure must not reboot the board** — it marks entities unavailable and
      keeps Wi-Fi up so you can see *that* it failed. Only a prolonged Wi-Fi/MQTT dead state
      (~15 min after backoff exhaustion) triggers a restart.

---

# Phase 6 — Optional

- [ ] CAN output to the inverter (pins 15/16 free, driver unused)
- [ ] Local web dashboard

Both have pins and headroom already. Inverter emulation over CAN is a much bigger commitment
than it looks — see the plan's exclusions.

---

# Permanently out of scope

**Writes to the BMS.** Function `0x10` can flip charge/discharge MOSFETs and change protection
setpoints. A bug in a loop that occasionally writes is a fire-adjacent bug, so the firmware has
**no write path at all** — not a disabled one. Since Track A this is **structural**: the UART
is opened with no TX pin and DE is held low as a plain GPIO, so the board physically cannot put
a byte on the wire.

If that ever changes, the shape is: a compile-time flag, a separate `.../cmd` topic, an
allow-list of writable registers, and read-back-and-verify after every write — plus routing a
TX pin, which is deliberately a conscious act rather than a config toggle.

---

# Troubleshooting

| Symptom | Cause / fix |
|---|---|
| `idf.py: command not found` | You didn't run `get_idf` in this terminal. Every window, every time. |
| `CONFIG_JK_SOMETHING undeclared` | A Kconfig bool that's off is absent, not `0`. Wrap it in `#ifdef` — see the menu section. |
| No new `/dev/cu.*` after plugging in | Charge-only USB-C cable (most likely), bad port, or bad hub. |
| `Failed to connect to ESP32-S3` | Hold BOOT, tap RESET, release BOOT, retry. |
| `Permission denied` on the port | Another program holds it — a previous `idf.py monitor`, or the Arduino IDE. |
| Monitor shows garbage | Usually the wrong port, not the wrong baud — `idf.py monitor` sets 115200 itself. |
| Poll times out, no response at all | Wrong slave address, or the V19 protocol field silently reverted to something other than `001`. |
| Poll returns exception `0x02` | Address is right, register is wrong. That's the map, not the wiring. |
| Sniffer sees nothing, warns every 10s | Nothing else is polling that port. Expected on a dedicated bus — use the poller. |
| Pack voltage absurd, cells fine | u32 word order. Flip the word-swap toggle. |
| `sed -i` errors on macOS | BSD sed needs `-i ''`. Linux syntax fails here. |

---

# Reference

- Board pinout: RS485 TX **17**, RX **18**, DE/RE **21**; CAN 15/16 (unused)
- V19 specifics and bugs: `syssi/esphome-jk-bms` discussion #747;
  `jean-luc1203/jkbms-rs485-addon` discussion #59
- RJ45 pinouts: jkbms.net → solutions/inverter-bms-communication
- Full rationale and architecture: `jk-bms-esp32s3-firmware-plan.md`
