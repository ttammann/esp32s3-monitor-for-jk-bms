// JK "55AA" frame protocol — the push-mode stream the BMS emits on RS485B /
// RS485C with the DIP switches at 0000.
//
// The BMS masters its own bus and transmits unprompted, so a listen-only build
// captures the whole dataset without ever putting a byte on the wire. That is
// the entire reason this protocol won over the Modbus register map, which has
// since been deleted.
//
// Field offsets came from a vendor-adjacent document written for
// JK_PB1A16S10P (fancyui/Gobel-Battery-HA-Integration,
// JK-BMS-55AA-Protocol_EN.md).
//
// STATUS: confirmed on hardware 2026-08-31 (PB2A16S20P, hw V19A / sw V19.31).
// Five independent cross-checks agree with the JK app: summed cell voltages vs
// reported pack voltage (52390 vs 52388 mV), V x I vs reported power (84.6 vs
// 84.606 W), remaining/full capacity vs reported SOC (61.9% vs 62%), decoded
// runtime vs the app's total (73.16 vs 72D15H20M), and computed vs reported
// cell delta (2 vs 2 mV). Those come from unrelated byte offsets, so agreement
// is evidence the offsets are right rather than an artefact of the arithmetic.
//
// No ESP-IDF dependencies, so it is host-testable.
#pragma once

#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>

#define JK55_MAX_CELLS   32
#define JK55_HDR_LEN     4        // 55 AA EB 90
#define JK55_MAX_FRAME   320

// Which trailing integrity check validated a frame.
//
// RESOLVED on hardware 2026-08-31: frames are **300 bytes with an additive
// sum8**, immediately followed by a separate frame-request write (see
// jk_req.h). That write is what the source document was counting when it
// described a "308-byte frame with a trailing CRC16" -- it is really two things
// on the wire, and the demux reports them as two. This holds for SETTINGS
// frames too: the document claims those carry 24 extra tail bytes, and on this
// unit they do not. JK55_CK_CRC16 is kept for other firmware revisions but has
// never been seen here.
typedef enum {
    JK55_CK_NONE = 0,
    JK55_CK_SUM8,      // 300-byte frame, last byte = sum of the first 299
    JK55_CK_CRC16,     // 308-byte frame, trailing little-endian CRC16
} jk55_check_t;

// Byte 4 distinguishes the two frame kinds.
//
// The vendor document calls this a "frame sequence number (01/02)" and the
// reference Python implementation uses it as a pack id, so neither treats it as
// a type field. On this unit it is nonetheless an exact discriminator, and the
// capture says why: the BMS issues its two request registers at a strict 1:1
// ratio (559 of 0x161E and 559 of 0x1620 over 220 s) and the answering frames
// come back at the same 1:1 ratio (35 with byte 4 = 0x01, 35 with 0x02). Each
// value pairs with the request that provoked it.
//
// That equivalence is a property of a single-pack bus. Add a second pack and
// byte 4 may well become the pack address the other two sources describe, at
// which point typing has to move to the request register in jk_req.h.
#define JK55_TYPE_SETTINGS  0x01   // answers a 0x161E request
#define JK55_TYPE_DYNAMIC   0x02   // answers a 0x1620 request

// The vendor document lists a third register, 0x161C ("static"). This BMS never
// requests it, so no frame type is defined for it here.

typedef struct {
    // pack
    uint32_t pack_mv;
    int32_t  current_ma;
    // Reported by the BMS, not derived -- and reported as an unsigned
    // MAGNITUDE: it reads positive while discharging. Take the direction from
    // current_ma. jk_fmt_state_json() publishes it signed for this reason.
    uint32_t power_mw;
    uint8_t  soc_pct;
    uint8_t  soh_pct;
    int32_t  remain_mah;
    uint32_t full_mah;
    uint32_t cycles;
    uint32_t cycle_mah;
    uint32_t runtime_s;
    // cells
    uint16_t cell_mv[JK55_MAX_CELLS];
    uint8_t  cell_count;        // popcount of the presence bitmap
    uint32_t cell_present;      // bitmap, offset 70
    uint16_t cell_avg_mv;       // reported by the BMS
    uint16_t cell_delta_mv;     // reported by the BMS
    uint8_t  cell_max_idx;      // 0-based
    uint8_t  cell_min_idx;
    int16_t  wire_res_mohm[JK55_MAX_CELLS];
    // thermal
    int16_t  mos_temp_dc;
    int16_t  t1_dc, t2_dc;
    // state
    int16_t  balance_ma;
    uint32_t alarms;
    bool     chg_mos, dsg_mos, bal_mos;
    uint8_t  battery_status;
} jk55_dynamic_t;

// The SETTINGS frame: what the BMS is *configured* to do, as opposed to the
// DYNAMIC frame's what-it-is-doing. One arrives per scan cycle alongside every
// dynamic frame, so this is half the available data.
//
// Every field below is a little-endian 32-bit value in the source document.
// Current limits are signed here even where the document types them unsigned:
// a limit that decodes negative means a wrong offset, and that should be
// visible rather than reappearing as an implausibly large positive.
typedef struct {
    // voltage thresholds, mV
    uint32_t sleep_mv;
    uint32_t cell_uvp_mv, cell_uvp_recover_mv;
    uint32_t cell_ovp_mv, cell_ovp_recover_mv;
    uint32_t balance_trigger_mv;      // cell spread that starts balancing
    uint32_t balance_start_mv;        // minimum cell voltage before balancing
    uint32_t soc100_mv, soc0_mv;
    uint32_t charge_max_mv, float_mv, power_off_mv;
    // current limits
    int32_t  charge_current_ma, discharge_current_ma;
    uint32_t max_balance_current_ma;
    // protection timing, seconds
    uint32_t charge_ocp_delay_s, charge_ocp_release_s;
    uint32_t discharge_ocp_delay_s, discharge_ocp_release_s;
    uint32_t scp_release_s, scp_delay_us;
    // temperature thresholds, 0.1 C
    int32_t  charge_otp_dc, charge_otp_recover_dc;
    int32_t  discharge_otp_dc, discharge_otp_recover_dc;
    int32_t  charge_utp_dc, charge_utp_recover_dc;
    int32_t  mos_otp_dc, mos_otp_recover_dc;
    // pack configuration
    uint32_t cell_count;
    uint32_t design_capacity_mah;
    uint32_t device_addr;
    uint32_t precharge_delay_s;
    bool     charge_enabled, discharge_enabled, balance_enabled;
} jk55_settings_t;

// Identify a 55AA frame starting at buf[0].
//   > 0  frame length, integrity check passed (*ck says which)
//     0  might still be a frame, feed more bytes
//   < 0  not a frame start, drop a byte and resync
// `ck` may be NULL.
int jk55_frame_len(const uint8_t *buf, size_t len, jk55_check_t *ck);

// Decode a DYNAMIC (byte 4 = 0x02) frame validated by jk55_frame_len. Returns
// false if the frame is too short or is not a dynamic frame.
bool jk55_decode_dynamic(const uint8_t *buf, size_t len, jk55_dynamic_t *d);

// Decode a SETTINGS (byte 4 = 0x01) frame. Same contract.
bool jk55_decode_settings(const uint8_t *buf, size_t len, jk55_settings_t *s);

// Power with a direction: negative while discharging, positive while charging.
//
// The BMS reports power as an unsigned magnitude (see power_mw above), so the
// sign has to come from current_ma. This exists as one function rather than
// the arithmetic being repeated because the console line and the MQTT payload
// both need it, and two copies would eventually disagree about which way the
// energy was flowing. Returns 0 for a NULL argument.
int32_t jk55_power_mw_signed(const jk55_dynamic_t *d);
