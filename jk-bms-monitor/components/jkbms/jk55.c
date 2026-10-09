#include <string.h>
#include "jk55.h"
#include "jk_crc.h"        // for the CRC16 frame variant

// --- little-endian readers -------------------------------------------------
// This protocol is little-endian throughout.

static uint16_t le16(const uint8_t *b)
{
    return (uint16_t)(b[0] | (b[1] << 8));
}

static uint32_t le32(const uint8_t *b)
{
    return (uint32_t)b[0] | ((uint32_t)b[1] << 8) |
           ((uint32_t)b[2] << 16) | ((uint32_t)b[3] << 24);
}

static int32_t le32s(const uint8_t *b)
{
    return (int32_t)le32(b);
}

static uint8_t popcount32(uint32_t v)
{
    uint8_t n = 0;
    while (v) { n += (uint8_t)(v & 1u); v >>= 1; }
    return n;
}

// --- framing ---------------------------------------------------------------

static const uint8_t JK55_MAGIC[JK55_HDR_LEN] = { 0x55, 0xAA, 0xEB, 0x90 };

// 300-byte variant: last byte is the 8-bit sum of everything before it.
static bool sum8_ok(const uint8_t *b, size_t len)
{
    uint8_t s = 0;
    for (size_t i = 0; i < len - 1; i++) {
        s = (uint8_t)(s + b[i]);
    }
    return s == b[len - 1];
}

int jk55_frame_len(const uint8_t *buf, size_t len, jk55_check_t *ck)
{
    if (ck) {
        *ck = JK55_CK_NONE;
    }
    // Match as much of the magic as we have; a prefix is "keep feeding me".
    for (size_t i = 0; i < JK55_HDR_LEN; i++) {
        if (i >= len) {
            return 0;
        }
        if (buf[i] != JK55_MAGIC[i]) {
            return -1;
        }
    }

    // Try the shorter first so a 300-byte frame is not held hostage waiting for
    // 308 bytes that will never arrive.
    if (len >= 300 && sum8_ok(buf, 300)) {
        if (ck) { *ck = JK55_CK_SUM8; }
        return 300;
    }
    if (len >= 308 && jk_crc16_ok(buf, 308)) {
        if (ck) { *ck = JK55_CK_CRC16; }
        return 308;
    }
    if (len < 308) {
        return 0;                  // header is right, wait for the body
    }
    return -1;                     // full-length candidate, neither check fits
}

// --- decode ----------------------------------------------------------------

bool jk55_decode_dynamic(const uint8_t *b, size_t len, jk55_dynamic_t *d)
{
    if (len < 300 || b[3] != 0x90) {
        return false;
    }
    if (b[4] != JK55_TYPE_DYNAMIC) {
        return false;
    }
    // Zeroed first so padding and any field not decoded below are canonical,
    // as for jk55_decode_settings.
    memset(d, 0, sizeof(*d));

    for (int i = 0; i < JK55_MAX_CELLS; i++) {
        d->cell_mv[i]       = le16(b + 6 + i * 2);
        d->wire_res_mohm[i] = (int16_t)le16(b + 80 + i * 2);
    }
    d->cell_present  = le32(b + 70);
    d->cell_count    = popcount32(d->cell_present);
    d->cell_avg_mv   = le16(b + 74);
    d->cell_delta_mv = le16(b + 76);
    d->cell_max_idx  = b[78];
    d->cell_min_idx  = b[79];

    d->mos_temp_dc = (int16_t)le16(b + 144);
    d->pack_mv     = le32(b + 150);
    d->power_mw    = le32(b + 154);
    d->current_ma  = le32s(b + 158);
    d->t1_dc       = (int16_t)le16(b + 162);
    d->t2_dc       = (int16_t)le16(b + 164);
    d->alarms      = le32(b + 166);
    d->balance_ma  = (int16_t)le16(b + 170);

    d->battery_status = b[172];
    d->soc_pct        = b[173];
    d->remain_mah     = le32s(b + 174);
    d->full_mah       = le32(b + 178);
    d->cycles         = le32(b + 182);
    d->cycle_mah      = le32(b + 186);
    d->soh_pct        = b[190];
    d->runtime_s      = le32(b + 194);

    d->chg_mos = b[198] != 0;
    d->dsg_mos = b[199] != 0;
    d->bal_mos = b[200] != 0;
    return true;
}

int32_t jk55_power_mw_signed(const jk55_dynamic_t *d)
{
    if (d == NULL) {
        return 0;
    }
    // Clamp before negating: power_mw is unsigned and a corrupt frame that
    // still passes sum8 could carry a magnitude past INT32_MAX, where the cast
    // would be implementation-defined and the negation undefined.
    const uint32_t mag = (d->power_mw > (uint32_t)INT32_MAX)
                       ? (uint32_t)INT32_MAX : d->power_mw;
    return (d->current_ma < 0) ? -(int32_t)mag : (int32_t)mag;
}

bool jk55_decode_settings(const uint8_t *b, size_t len, jk55_settings_t *s)
{
    if (len < 300 || b[3] != 0x90) {
        return false;
    }
    if (b[4] != JK55_TYPE_SETTINGS) {
        return false;
    }
    // Zeroed first so padding is canonical whatever the caller's storage:
    // listen.c dedupes decoded settings with memcmp.
    memset(s, 0, sizeof(*s));

    s->sleep_mv             = le32(b + 6);
    s->cell_uvp_mv          = le32(b + 10);
    s->cell_uvp_recover_mv  = le32(b + 14);
    s->cell_ovp_mv          = le32(b + 18);
    s->cell_ovp_recover_mv  = le32(b + 22);
    s->balance_trigger_mv   = le32(b + 26);
    s->soc100_mv            = le32(b + 30);
    s->soc0_mv              = le32(b + 34);
    s->charge_max_mv        = le32(b + 38);
    s->float_mv             = le32(b + 42);
    s->power_off_mv         = le32(b + 46);

    s->charge_current_ma      = le32s(b + 50);
    s->charge_ocp_delay_s     = le32(b + 54);
    s->charge_ocp_release_s   = le32(b + 58);
    s->discharge_current_ma   = le32s(b + 62);
    s->discharge_ocp_delay_s  = le32(b + 66);
    s->discharge_ocp_release_s = le32(b + 70);
    s->scp_release_s          = le32(b + 74);
    s->max_balance_current_ma = le32(b + 78);

    s->charge_otp_dc            = le32s(b + 82);
    s->charge_otp_recover_dc    = le32s(b + 86);
    s->discharge_otp_dc         = le32s(b + 90);
    s->discharge_otp_recover_dc = le32s(b + 94);
    s->charge_utp_dc            = le32s(b + 98);
    s->charge_utp_recover_dc    = le32s(b + 102);
    s->mos_otp_dc               = le32s(b + 106);
    s->mos_otp_recover_dc       = le32s(b + 110);

    s->cell_count          = le32(b + 114);
    s->charge_enabled      = le32(b + 118) != 0;
    s->discharge_enabled   = le32(b + 122) != 0;
    s->balance_enabled     = le32(b + 126) != 0;
    s->design_capacity_mah = le32(b + 130);
    s->scp_delay_us        = le32(b + 134);
    s->balance_start_mv    = le32(b + 138);
    // 142..269 is a 32-entry wire-resistance calibration table in micro-ohms.
    // The dynamic frame already carries measured wire resistances, so the
    // calibration values are skipped rather than stored twice.
    s->device_addr         = le32(b + 270);
    s->precharge_delay_s   = le32(b + 274);
    return true;
}
