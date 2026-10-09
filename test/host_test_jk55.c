// Host tests for the JK 55AA push-protocol framer and decoders.
//
// These build synthetic frames from the documented field offsets, so they
// verify the decoder against the specification we implemented from. They
// cannot verify the specification itself against real hardware -- that is what
// the capture cross-checks in jk55.h record.
#include <stdio.h>
#include <string.h>
#include "jk55.h"
#include "jk_crc.h"
#include "test_util.h"

static void put16(uint8_t *b, size_t off, uint16_t v)
{
    b[off] = (uint8_t)v; b[off + 1] = (uint8_t)(v >> 8);
}

static void put32(uint8_t *b, size_t off, uint32_t v)
{
    b[off]     = (uint8_t)v;         b[off + 1] = (uint8_t)(v >> 8);
    b[off + 2] = (uint8_t)(v >> 16); b[off + 3] = (uint8_t)(v >> 24);
}

static uint8_t sum8_of(const uint8_t *b, size_t upto)
{
    uint8_t s = 0;
    for (size_t i = 0; i < upto; i++) { s = (uint8_t)(s + b[i]); }
    return s;
}

static void seal_sum8(uint8_t *b)            // 300-byte variant
{
    b[299] = sum8_of(b, 299);
}

static void seal_crc16(uint8_t *b)           // 308-byte variant
{
    const uint16_t c = jk_crc16(b, 306);
    b[306] = (uint8_t)c; b[307] = (uint8_t)(c >> 8);
}

// A plausible 16S LiFePO4 pack at rest.
static void build_dynamic(uint8_t *b)
{
    memset(b, 0, JK55_MAX_FRAME);
    b[0] = 0x55; b[1] = 0xAA; b[2] = 0xEB; b[3] = 0x90;
    b[4] = JK55_TYPE_DYNAMIC;
    b[5] = 0x00;

    for (int i = 0; i < 16; i++) {
        put16(b, 6 + (size_t)i * 2, (uint16_t)(3320 + i));
        put16(b, 80 + (size_t)i * 2, (uint16_t)(250 + i));   // wire mOhm
    }
    put32(b, 70, 0x0000FFFFu);          // 16 cells present
    put16(b, 74, 3327);                 // avg
    put16(b, 76, 15);                   // delta
    b[78] = 15;                         // max cell index (0-based)
    b[79] = 0;                          // min cell index

    put16(b, 144, 232);                 // MOS 23.2 C
    put32(b, 150, 53248);               // 53.248 V
    put32(b, 154, 133120);              // 133.12 W
    put32(b, 158, (uint32_t)-2500);     // -2.5 A
    put16(b, 162, 210);
    put16(b, 164, (uint16_t)-55);       // -5.5 C, sub-zero sensor
    put32(b, 166, 0x00000004u);         // an alarm bit
    put16(b, 170, (uint16_t)-120);      // balancing, -120 mA

    b[172] = 0x01;                      // battery status
    b[173] = 87;                        // SOC
    put32(b, 174, 208000);              // remaining mAh
    put32(b, 178, 240000);              // full mAh
    put32(b, 182, 143);                 // cycles
    put32(b, 186, 34320000);            // total cycle capacity
    b[190] = 99;                        // SOH
    put32(b, 194, 8640000);             // runtime s

    b[198] = 1; b[199] = 1; b[200] = 0; // chg on, dsg on, balance off
}

// A settings frame with values a 16S 280 Ah pack would plausibly carry.
static void build_settings(uint8_t *b)
{
    memset(b, 0, JK55_MAX_FRAME);
    b[0] = 0x55; b[1] = 0xAA; b[2] = 0xEB; b[3] = 0x90;
    b[4] = JK55_TYPE_SETTINGS;
    b[5] = 0x00;

    put32(b, 6,   3000);        // sleep entry mV
    put32(b, 10,  2500);        // cell UVP
    put32(b, 14,  2900);        // cell UVP recover
    put32(b, 18,  3650);        // cell OVP
    put32(b, 22,  3400);        // cell OVP recover
    put32(b, 26,  10);          // balance trigger diff mV
    put32(b, 30,  3550);        // SOC 100% mV
    put32(b, 34,  2900);        // SOC 0% mV
    put32(b, 38,  56000);       // inverter max charge mV
    put32(b, 42,  54000);       // float mV
    put32(b, 46,  2400);        // auto power-off mV

    put32(b, 50,  (uint32_t)200000);   // continuous charge mA
    put32(b, 54,  30);                 // charge OCP delay s
    put32(b, 58,  60);                 // charge OCP release s
    put32(b, 62,  (uint32_t)200000);   // continuous discharge mA
    put32(b, 66,  30);
    put32(b, 70,  60);
    put32(b, 74,  10);                 // SCP release s
    put32(b, 78,  1000);               // max balance mA

    put32(b, 82,  700);                // charge OTP 70.0 C
    put32(b, 86,  650);
    put32(b, 90,  750);
    put32(b, 94,  700);
    put32(b, 98,  (uint32_t)-100);     // charge low-temp -10.0 C
    put32(b, 102, 0);
    put32(b, 106, 900);                // MOS OTP 90.0 C
    put32(b, 110, 800);

    put32(b, 114, 16);                 // cell count
    put32(b, 118, 1);                  // charge switch
    put32(b, 122, 1);                  // discharge switch
    put32(b, 126, 0);                  // balance switch off
    put32(b, 130, 280000);             // design capacity mAh
    put32(b, 134, 200);                // SCP delay us
    put32(b, 138, 3400);               // balance start mV
    put32(b, 270, 1);                  // device address
    put32(b, 274, 5);                  // precharge delay s
}

static void test_frame_sum8(void)
{
    uint8_t b[JK55_MAX_FRAME];
    jk55_check_t ck = JK55_CK_NONE;
    build_dynamic(b);
    seal_sum8(b);

    EQ_I(jk55_frame_len(b, 300, &ck), 300);
    EQ_I(ck, JK55_CK_SUM8);
    // Partial input must ask for more rather than guess a length.
    EQ_I(jk55_frame_len(b, 4, &ck), 0);
    EQ_I(jk55_frame_len(b, 299, &ck), 0);
}

// This test used to bail out with `if (sum8 happened to pass) return;`, which
// meant it silently asserted nothing about 1 frame in 256. Force sum8 to fail
// by construction instead, so the CRC16 path is always the one exercised.
static void test_frame_crc16(void)
{
    uint8_t b[JK55_MAX_FRAME];
    jk55_check_t ck = JK55_CK_NONE;
    build_dynamic(b);
    b[299] = (uint8_t)(sum8_of(b, 299) + 1u);   // deterministically wrong
    seal_crc16(b);

    EQ_I(jk55_frame_len(b, 300, &ck), 0);       // not sum8, and not yet 308
    EQ_I(jk55_frame_len(b, 308, &ck), 308);
    EQ_I(ck, JK55_CK_CRC16);
}

static void test_frame_rejects(void)
{
    uint8_t b[JK55_MAX_FRAME];
    jk55_check_t ck = JK55_CK_NONE;
    build_dynamic(b);
    seal_sum8(b);

    b[0] = 0x56;                                    // wrong magic
    EQ_I(jk55_frame_len(b, 300, &ck), -1);
    b[0] = 0x55; b[2] = 0xEC;
    EQ_I(jk55_frame_len(b, 300, &ck), -1);

    build_dynamic(b); seal_sum8(b);
    b[150] ^= 0xFF;                                 // corrupt the payload
    EQ_I(jk55_frame_len(b, 308, &ck), -1);
    EQ_I(ck, JK55_CK_NONE);
}

// A prefix of the magic is "keep feeding me", not a rejection -- otherwise a
// frame split across two UART reads is lost.
static void test_frame_partial_magic(void)
{
    const uint8_t p[3] = { 0x55, 0xAA, 0xEB };
    EQ_I(jk55_frame_len(p, 1, NULL), 0);
    EQ_I(jk55_frame_len(p, 2, NULL), 0);
    EQ_I(jk55_frame_len(p, 3, NULL), 0);
}

static void test_decode_dynamic(void)
{
    uint8_t b[JK55_MAX_FRAME];
    jk55_dynamic_t d;
    memset(&d, 0, sizeof d);
    build_dynamic(b);
    seal_sum8(b);

    CHECK(jk55_decode_dynamic(b, 300, &d));
    EQ_U(d.pack_mv, 53248);
    EQ_I(d.current_ma, -2500);
    EQ_U(d.power_mw, 133120);
    EQ_U(d.soc_pct, 87);
    EQ_U(d.soh_pct, 99);
    EQ_I(d.remain_mah, 208000);
    EQ_U(d.full_mah, 240000);
    EQ_U(d.cycles, 143);
    EQ_U(d.cycle_mah, 34320000);
    EQ_U(d.runtime_s, 8640000);

    EQ_U(d.cell_present, 0x0000FFFFu);
    EQ_U(d.cell_count, 16);
    EQ_U(d.cell_mv[0], 3320);
    EQ_U(d.cell_mv[15], 3335);
    EQ_U(d.cell_mv[16], 0);
    EQ_U(d.cell_avg_mv, 3327);
    EQ_U(d.cell_delta_mv, 15);
    EQ_U(d.cell_max_idx, 15);
    EQ_U(d.cell_min_idx, 0);
    EQ_I(d.wire_res_mohm[0], 250);
    EQ_I(d.wire_res_mohm[15], 265);

    EQ_I(d.mos_temp_dc, 232);
    EQ_I(d.t1_dc, 210);
    EQ_I(d.t2_dc, -55);              // signed, sub-zero
    EQ_U(d.alarms, 0x00000004u);
    EQ_I(d.balance_ma, -120);        // signed, discharging balance
    EQ_U(d.battery_status, 1);
    CHECK(d.chg_mos);
    CHECK(d.dsg_mos);
    CHECK(!d.bal_mos);
}

static void test_decode_settings(void)
{
    uint8_t b[JK55_MAX_FRAME];
    jk55_settings_t s;
    memset(&s, 0, sizeof s);
    build_settings(b);
    seal_sum8(b);

    CHECK(jk55_decode_settings(b, 300, &s));
    EQ_U(s.cell_count, 16);
    EQ_U(s.design_capacity_mah, 280000);
    EQ_U(s.device_addr, 1);
    EQ_U(s.cell_ovp_mv, 3650);
    EQ_U(s.cell_ovp_recover_mv, 3400);
    EQ_U(s.cell_uvp_mv, 2500);
    EQ_U(s.cell_uvp_recover_mv, 2900);
    EQ_U(s.balance_trigger_mv, 10);
    EQ_U(s.balance_start_mv, 3400);
    EQ_U(s.soc100_mv, 3550);
    EQ_U(s.soc0_mv, 2900);
    EQ_U(s.float_mv, 54000);
    EQ_U(s.charge_max_mv, 56000);
    EQ_I(s.charge_current_ma, 200000);
    EQ_I(s.discharge_current_ma, 200000);
    EQ_U(s.max_balance_current_ma, 1000);
    EQ_I(s.charge_otp_dc, 700);
    EQ_I(s.mos_otp_dc, 900);
    EQ_I(s.charge_utp_dc, -100);     // signed, sub-zero
    EQ_U(s.scp_delay_us, 200);
    EQ_U(s.precharge_delay_s, 5);
    CHECK(s.charge_enabled);
    CHECK(s.discharge_enabled);
    CHECK(!s.balance_enabled);
}

// Each decoder must refuse the other's frame type, or a settings frame would be
// read as a dynamic one at completely unrelated offsets.
static void test_decoders_reject_wrong_type(void)
{
    uint8_t b[JK55_MAX_FRAME];
    jk55_dynamic_t d;
    jk55_settings_t s;
    memset(&d, 0, sizeof d);
    memset(&s, 0, sizeof s);

    build_dynamic(b); seal_sum8(b);
    CHECK(!jk55_decode_dynamic(b, 299, &d));        // short
    CHECK(!jk55_decode_settings(b, 300, &s));       // dynamic frame

    build_settings(b); seal_sum8(b);
    CHECK(!jk55_decode_dynamic(b, 300, &d));        // settings frame
    CHECK(jk55_decode_settings(b, 300, &s));
}

// Cell presence is a bitmap, not a count: a pack with a gap must not report a
// contiguous run.
static void test_cell_presence_bitmap(void)
{
    uint8_t b[JK55_MAX_FRAME];
    jk55_dynamic_t d;
    memset(&d, 0, sizeof d);
    build_dynamic(b);
    put32(b, 70, 0x0000FF0Fu);      // 4 + 8 populated, with a hole
    seal_sum8(b);

    CHECK(jk55_decode_dynamic(b, 300, &d));
    EQ_U(d.cell_count, 12);
    EQ_U(d.cell_present, 0x0000FF0Fu);
}

// The BMS reports power as an unsigned magnitude, so the direction has to come
// from current. Both the console line and the MQTT payload go through this, so
// it is the one place the rule can be got wrong.
static void test_power_sign(void)
{
    jk55_dynamic_t d;
    memset(&d, 0, sizeof d);

    d.power_mw = 133120;
    d.current_ma = -2500;                  // discharging
    EQ_I(jk55_power_mw_signed(&d), -133120);
    d.current_ma = 2500;                   // charging
    EQ_I(jk55_power_mw_signed(&d), 133120);
    d.current_ma = 0;                      // at rest
    EQ_I(jk55_power_mw_signed(&d), 133120);

    // At rest with no power is the common idle case and must not produce a
    // negative zero.
    d.power_mw = 0;
    d.current_ma = 0;
    EQ_I(jk55_power_mw_signed(&d), 0);

    // A corrupt frame that still passes sum8 could carry a magnitude past
    // INT32_MAX, where casting then negating would be undefined. Clamped.
    d.power_mw = 0xFFFFFFFFu;
    d.current_ma = -1;
    EQ_I(jk55_power_mw_signed(&d), -2147483647);
    d.current_ma = 1;
    EQ_I(jk55_power_mw_signed(&d), 2147483647);

    EQ_I(jk55_power_mw_signed(NULL), 0);
}

// --- stream reassembly, via the shared harness ------------------------------

static th_action_t classify_jk55(const uint8_t *buf, size_t n, bool stalled,
                                 size_t *len_out, int *kind_out, void *ctx)
{
    (void)ctx; (void)kind_out;
    const int fl = jk55_frame_len(buf, n, NULL);
    if (fl > 0) {
        *len_out = (size_t)fl;
        return TH_ITEM;
    }
    if (fl == 0 && !stalled) {
        return TH_NEED_MORE;
    }
    return TH_DROP;
}

// The stream is a back-to-back sequence of frames with no delimiter but the
// magic, delivered in arbitrary chunk sizes.
static void test_stream_reassembly(void)
{
    uint8_t one[JK55_MAX_FRAME];
    build_dynamic(one);
    seal_sum8(one);

    uint8_t s[1024];
    size_t n = 0;
    memcpy(s + n, one, 300); n += 300;
    s[n++] = 0xFF; s[n++] = 0x13;                   // noise between frames
    memcpy(s + n, one, 300); n += 300;

    uint8_t buf[640];
    const size_t chunks[] = {1, 5, 290, 3, 100, 400};
    const th_result_t r = th_run(s, n, chunks, 6, buf, sizeof buf,
                                 classify_jk55, NULL);
    EQ_I(r.items, 2);
    EQ_I(r.dropped, 2);       // exactly the two noise bytes
}

int main(void)
{
    test_frame_sum8();
    test_frame_crc16();
    test_frame_rejects();
    test_frame_partial_magic();
    test_decode_dynamic();
    test_decode_settings();
    test_decoders_reject_wrong_type();
    test_cell_presence_bitmap();
    test_power_sign();
    test_stream_reassembly();
    return test_report("jk55");
}
