// Host tests for the bounded string builder, the protocol demultiplexer, the
// bus diagnosis and the console/JSON formatting.
//
// All four exist because the same class of bug appeared in hand-written code:
// an snprintf cursor walking past the end of a buffer, a frame classifier that
// could wedge or mis-frame, a verdict that reported a healthy link on a dead
// bus, and raw indexing over wire-supplied offsets.
#include <stdio.h>
#include <string.h>
#include "strbuf.h"
#include "jk_demux.h"
#include "jk_req.h"
#include "jk_crc.h"
#include "jk_busdiag.h"
#include "jk_fmt.h"
#include "test_util.h"

// --- strbuf ---------------------------------------------------------------

static void test_sb_basic(void)
{
    char b[32];
    int w = sb_addf(b, sizeof b, 0, "{\"a\":%d,", 12);
    w = sb_addf(b, sizeof b, w, "\"b\":%d}", 34);
    CHECK(!sb_overflowed(sizeof b, w));
    EQ_I(w, (int)strlen(b));
    CHECK(strcmp(b, "{\"a\":12,\"b\":34}") == 0);
}

// The bug this header exists to prevent: once a write truncates, the cursor
// must stop at "full" instead of running past the end of the buffer.
static void test_sb_truncation_clamps(void)
{
    char b[8];
    int w = sb_addf(b, sizeof b, 0, "%s", "0123456789abcdef");
    EQ_I(w, (int)sizeof b);             // clamped, not 16
    CHECK(sb_overflowed(sizeof b, w));
    EQ_U(strlen(b), sizeof b - 1);      // still NUL-terminated

    // Further appends must be no-ops, not out-of-bounds writes. ASan is the
    // real assertion here.
    w = sb_addf(b, sizeof b, w, "more");
    EQ_I(w, (int)sizeof b);
    EQ_U(strlen(b), sizeof b - 1);
}

static void test_sb_exact_fit(void)
{
    char b[6];
    int w = sb_addf(b, sizeof b, 0, "%s", "12345");
    EQ_I(w, 5);
    CHECK(!sb_overflowed(sizeof b, w));
    CHECK(strcmp(b, "12345") == 0);

    w = sb_addf(b, sizeof b, 0, "%s", "123456");
    CHECK(sb_overflowed(sizeof b, w));
}

static void test_sb_degenerate(void)
{
    char b[4];
    EQ_I(sb_addf(b, 0, 0, "x"), 0);            // zero-size buffer
    EQ_I(sb_addf(NULL, sizeof b, 0, "x"), (int)sizeof b);
    EQ_I(sb_addf(b, sizeof b, -1, "x"), (int)sizeof b);   // negative cursor
    EQ_I(sb_addf(b, sizeof b, 99, "x"), (int)sizeof b);   // cursor past end
}

// --- fixtures ---------------------------------------------------------------

static size_t mk_req(uint8_t *b, uint8_t pack, uint16_t reg)
{
    b[0] = pack; b[1] = 0x10;
    b[2] = (uint8_t)(reg >> 8); b[3] = (uint8_t)reg;
    b[4] = 0x00; b[5] = 0x01;
    const uint16_t c = jk_crc16(b, 6);
    b[6] = (uint8_t)c; b[7] = (uint8_t)(c >> 8);
    return 8;
}

static size_t mk_jk55(uint8_t *b, uint8_t type)
{
    memset(b, 0, 300);
    b[0] = 0x55; b[1] = 0xAA; b[2] = 0xEB; b[3] = 0x90; b[4] = type;
    uint8_t s = 0;
    for (size_t i = 0; i < 299; i++) { s = (uint8_t)(s + b[i]); }
    b[299] = s;
    return 300;
}

// --- demux ----------------------------------------------------------------

static void test_demux_req(void)
{
    uint8_t b[16];
    jk_demux_out_t out;
    mk_req(b, 0x00, JK_REQ_REG_DYNAMIC);
    EQ_I(jk_demux(b, 8, false, &out), JK_DEMUX_REQ);
    EQ_U(out.len, 8);
    EQ_U(out.req_reg, JK_REQ_REG_DYNAMIC);
    EQ_U(out.req_pack, 0x00);
}

static void test_demux_jk55(void)
{
    uint8_t b[320];
    jk_demux_out_t out;
    mk_jk55(b, JK55_TYPE_DYNAMIC);
    EQ_I(jk_demux(b, 300, false, &out), JK_DEMUX_JK55);
    EQ_U(out.len, 300);
    EQ_I(out.ck, JK55_CK_SUM8);
}

// The mis-framing bug: while a 55AA body is still arriving, the request
// recogniser must not be allowed to look at those bytes at all.
static void test_demux_partial_jk55_not_given_to_req(void)
{
    uint8_t b[320];
    mk_jk55(b, JK55_TYPE_DYNAMIC);
    for (size_t n = 4; n < 300; n += 37) {
        EQ_I(jk_demux(b, n, false, NULL), JK_DEMUX_NEED_MORE);
    }
}

// ...but a 55AA header whose body never arrives must not wedge the stream
// forever. Once stalled, it has to give up a byte and resync.
static void test_demux_stalled_jk55_resyncs(void)
{
    uint8_t b[320];
    mk_jk55(b, JK55_TYPE_DYNAMIC);
    EQ_I(jk_demux(b, 100, true, NULL), JK_DEMUX_DROP1);
    EQ_I(jk_demux(b, 100, false, NULL), JK_DEMUX_NEED_MORE);
}

static void test_demux_noise(void)
{
    const uint8_t noise[8] = {0xFF, 0x00, 0x5A, 0xA5, 0x11, 0x22, 0x33, 0x44};
    EQ_I(jk_demux(noise, 8, false, NULL), JK_DEMUX_DROP1);
}

static void test_demux_short(void)
{
    const uint8_t b[3] = {0x55, 0xAA, 0xEB};
    EQ_I(jk_demux(b, 3, false, NULL), JK_DEMUX_NEED_MORE);
    EQ_I(jk_demux(b, 3, true,  NULL), JK_DEMUX_DROP1);
    EQ_I(jk_demux(b, 0, true,  NULL), JK_DEMUX_NEED_MORE);
}

#define KIND_JK55 0
#define KIND_REQ  1

static th_action_t classify_demux(const uint8_t *buf, size_t n, bool stalled,
                                  size_t *len_out, int *kind_out, void *ctx)
{
    (void)ctx;
    jk_demux_out_t out;
    switch (jk_demux(buf, n, stalled, &out)) {
    case JK_DEMUX_JK55: *len_out = out.len; *kind_out = KIND_JK55; return TH_ITEM;
    case JK_DEMUX_REQ:  *len_out = out.len; *kind_out = KIND_REQ;  return TH_ITEM;
    case JK_DEMUX_DROP1: return TH_DROP;
    default: return TH_NEED_MORE;
    }
}

// A stream shaped like the real bus: request writes to several pack addresses,
// the answering frame, noise, then more requests.
static void test_demux_mixed_stream(void)
{
    uint8_t s[1024];
    size_t n = 0;
    n += mk_req(s + n, 0x00, JK_REQ_REG_SETTINGS);
    n += mk_jk55(s + n, JK55_TYPE_SETTINGS);
    n += mk_req(s + n, 0x00, JK_REQ_REG_DYNAMIC);
    n += mk_jk55(s + n, JK55_TYPE_DYNAMIC);
    s[n++] = 0xC3; s[n++] = 0x17;                     // line noise
    n += mk_req(s + n, 0x01, JK_REQ_REG_DYNAMIC);

    uint8_t buf[640];
    const size_t chunks[] = {3, 200, 17, 400, 2, 90};
    const th_result_t r = th_run(s, n, chunks, 6, buf, sizeof buf,
                                 classify_demux, NULL);
    EQ_I(r.kind[KIND_JK55], 2);
    EQ_I(r.kind[KIND_REQ], 3);
    EQ_I(r.dropped, 2);       // exactly the two noise bytes
}

// --- bus diagnosis --------------------------------------------------------

static void test_diag_silent(void)
{
    jk_busstats_t s = {0};
    EQ_I(jk_bus_diagnose(&s), JK_BUS_SILENT);
}

static void test_diag_noise_swapped_pair(void)
{
    jk_busstats_t s = {0};
    s.frame_err = 400;
    s.breaks = 12;
    EQ_I(jk_bus_diagnose(&s), JK_BUS_NOISE);

    jk_busstats_t t = {0};
    t.bytes = 100;
    t.frame_err = 90;
    EQ_I(jk_bus_diagnose(&t), JK_BUS_NOISE);
}

static void test_diag_unframed(void)
{
    jk_busstats_t s = {0};
    s.bytes = 2100;
    s.dropped = 2100;
    EQ_I(jk_bus_diagnose(&s), JK_BUS_UNFRAMED);
}

static void test_diag_few_errors_is_not_noise(void)
{
    jk_busstats_t s = {0};
    s.bytes = 1000;
    s.frame_err = 20;          // 2%, under the 5% threshold
    EQ_I(jk_bus_diagnose(&s), JK_BUS_UNFRAMED);

    s.frame_err = 60;          // 6%, over it
    EQ_I(jk_bus_diagnose(&s), JK_BUS_NOISE);
}

static void test_diag_ok_needs_real_frames(void)
{
    jk_busstats_t s = {0};
    s.bytes = 3000;
    s.frames_jk55 = 10;
    s.frame_err = 500;
    s.fifo_ovf = 3;
    EQ_I(jk_bus_diagnose(&s), JK_BUS_OK);
}

// The regression that mattered most: request writes are not battery data. A bus
// where the BMS is scanning but no pack answers used to count those requests as
// "frames" and report a healthy link.
static void test_diag_requests_without_answers(void)
{
    jk_busstats_t s = {0};
    s.bytes = 1100;
    s.frames_req = 50;         // a full scan cycle of requests
    s.frames_jk55 = 0;         // nobody answered
    EQ_I(jk_bus_diagnose(&s), JK_BUS_NO_DATA);

    // One real frame flips it back.
    s.frames_jk55 = 1;
    EQ_I(jk_bus_diagnose(&s), JK_BUS_OK);
}

static void test_diag_overrun(void)
{
    jk_busstats_t s = {0};
    s.bytes = 5000;
    s.buf_full = 2;
    EQ_I(jk_bus_diagnose(&s), JK_BUS_OVERRUN);
}

static void test_diag_advice_always_present(void)
{
    const jk_bus_verdict_t all[] = { JK_BUS_SILENT, JK_BUS_NOISE,
                                     JK_BUS_UNFRAMED, JK_BUS_OVERRUN,
                                     JK_BUS_NO_DATA, JK_BUS_OK };
    for (size_t i = 0; i < sizeof all / sizeof all[0]; i++) {
        const char *a = jk_bus_advice(all[i]);
        CHECK(a != NULL);
        CHECK(a[0] != '\0');
    }
}

// --- formatting -----------------------------------------------------------

static void fill_dynamic(jk55_dynamic_t *d)
{
    memset(d, 0, sizeof *d);
    for (int i = 0; i < 16; i++) { d->cell_mv[i] = (uint16_t)(3320 + i); }
    d->cell_present  = 0x0000FFFFu;
    d->cell_count    = 16;
    d->cell_avg_mv   = 3327;
    d->cell_delta_mv = 15;
    d->cell_min_idx  = 0;
    d->cell_max_idx  = 15;
    d->pack_mv    = 53248;
    d->current_ma = -2500;
    d->power_mw   = 133120;
    d->soc_pct    = 87;
    d->soh_pct    = 99;
    d->remain_mah = 208000;
    d->full_mah   = 240000;
    d->cycles     = 143;
    d->cycle_mah  = 34320000;
    d->runtime_s  = 8640000;
    d->mos_temp_dc = 232;
    d->t1_dc = 210;
    d->t2_dc = -55;
    for (int i = 0; i < 16; i++) { d->wire_res_mohm[i] = (int16_t)(250 + i); }
    // An unpopulated slot carrying a huge value must not win the maximum.
    d->wire_res_mohm[20] = 9999;
}

static void test_fmt_cell_rows(void)
{
    EQ_I(jk_fmt_cell_rows(0x00000000u), 0);
    EQ_I(jk_fmt_cell_rows(0x0000FFFFu), 2);
    EQ_I(jk_fmt_cell_rows(0xFFFFFFFFu), 4);
    EQ_I(jk_fmt_cell_rows(0x00000001u), 1);
    // A hole still prints the rows that span it: a missing cell is information.
    EQ_I(jk_fmt_cell_rows(0x0000FF0Fu), 2);
}

static void test_fmt_cell_row(void)
{
    jk55_dynamic_t d;
    fill_dynamic(&d);
    char line[8 * 5 + 1];

    EQ_I(jk_fmt_cell_row(line, sizeof line, d.cell_mv, JK55_MAX_CELLS, 0), 39);
    CHECK(strcmp(line, "3320 3321 3322 3323 3324 3325 3326 3327") == 0);
    CHECK(jk_fmt_cell_row(line, sizeof line, d.cell_mv, JK55_MAX_CELLS, 3) > 0);

    // Out of range, and a buffer far too small: both must fail cleanly rather
    // than write past the end. ASan is the real assertion.
    EQ_I(jk_fmt_cell_row(line, sizeof line, d.cell_mv, JK55_MAX_CELLS, 4), -1);
    EQ_I(jk_fmt_cell_row(line, sizeof line, d.cell_mv, JK55_MAX_CELLS, -1), -1);
    char tiny[4];
    EQ_I(jk_fmt_cell_row(tiny, sizeof tiny, d.cell_mv, JK55_MAX_CELLS, 0), -1);
    EQ_U(strlen(tiny), sizeof tiny - 1);          // still terminated
}

static void test_fmt_hex(void)
{
    const uint8_t b[4] = {0x55, 0xAA, 0xEB, 0x90};
    char out[16];
    jk_fmt_hex(out, sizeof out, b, 4);
    CHECK(strcmp(out, "55 AA EB 90") == 0);

    // Truncation must stop inside the buffer.
    char small[6];
    jk_fmt_hex(small, sizeof small, b, 4);
    EQ_U(strlen(small), sizeof small - 1);

    char one[1];
    jk_fmt_hex(one, sizeof one, b, 4);
    EQ_U(strlen(one), 0);
    jk_fmt_hex(out, sizeof out, b, 0);
    EQ_U(strlen(out), 0);
}

// cell_min_idx / cell_max_idx come straight off the wire. A corrupt frame that
// still passes sum8 could carry 0xFF, and that must not index the array.
static void test_fmt_cell_at_bounds(void)
{
    jk55_dynamic_t d;
    fill_dynamic(&d);
    EQ_U(jk_fmt_cell_at(&d, 0), 3320);
    EQ_U(jk_fmt_cell_at(&d, 15), 3335);
    EQ_U(jk_fmt_cell_at(&d, JK55_MAX_CELLS), 0);
    EQ_U(jk_fmt_cell_at(&d, 0xFF), 0);
    EQ_U(jk_fmt_cell_at(NULL, 0), 0);
}

static int count_char(const char *s, char c)
{
    int k = 0;
    for (; *s; s++) { if (*s == c) { k++; } }
    return k;
}

static void test_fmt_state_json(void)
{
    jk55_dynamic_t d;
    fill_dynamic(&d);
    char json[1536];

    const int w = jk_fmt_state_json(json, sizeof json, &d, -55, 1234, 7);
    CHECK(w > 0);
    EQ_I(w, (int)strlen(json));
    CHECK(json[0] == '{');
    CHECK(json[w - 1] == '}');
    CHECK(strstr(json, "\"pack_v\":53.248") != NULL);
    CHECK(strstr(json, "\"soc\":87") != NULL);
    CHECK(strstr(json, "\"cell_min_v\":3.320") != NULL);
    CHECK(strstr(json, "\"cell_max_v\":3.335") != NULL);
    CHECK(strstr(json, "\"rssi\":-55") != NULL);
    // Removed deliberately: its only producer hardcoded it true, so the entity
    // reading it could never report a disconnection. Freshness now travels on
    // the retained bms_status topic instead.
    CHECK(strstr(json, "comms_ok") == NULL);
    // The BMS reports power as an unsigned magnitude. fill_dynamic() is
    // discharging (-2.5 A), so the published figure must come out negative or
    // anything integrating it counts a discharge as energy going in.
    CHECK(strstr(json, "\"power_w\":-133.1") != NULL);
    // 265 is the worst among the 16 present cells; the 9999 parked in an
    // absent slot must be ignored.
    CHECK(strstr(json, "\"wire_res_max_mohm\":265") != NULL);

    // Up to the highest present cell: 16 slots means 15 separators.
    const char *cells = strstr(json, "\"cells\":[");
    CHECK(cells != NULL);
    EQ_I(count_char(cells, ','), 15);
}

// A hole in the presence bitmap must stay a hole. Compacting it out shifts
// every later cell onto the wrong Home Assistant entity.
static void test_fmt_state_json_cell_holes(void)
{
    jk55_dynamic_t d;
    fill_dynamic(&d);
    d.cell_present = 0x0000FF0Fu;     // cells 5-8 absent
    char json[1536];
    CHECK(jk_fmt_state_json(json, sizeof json, &d, 0, 0, 0) > 0);

    const char *cells = strstr(json, "\"cells\":[");
    CHECK(cells != NULL);
    // Slot 5 (index 4) is null and slot 9 (index 8) keeps cell 9's voltage.
    char want[96];
    snprintf(want, sizeof want, "\"cells\":[%.3f,%.3f,%.3f,%.3f,"
             "null,null,null,null,%.3f,",
             d.cell_mv[0] / 1000.0, d.cell_mv[1] / 1000.0,
             d.cell_mv[2] / 1000.0, d.cell_mv[3] / 1000.0,
             d.cell_mv[8] / 1000.0);
    CHECK(strncmp(cells, want, strlen(want)) == 0);
    EQ_I(count_char(cells, ','), 15);  // still 16 slots, up to cell 16

    // Absent top cells are trimmed, not padded with nulls.
    d.cell_present = 0x0000000Fu;
    CHECK(jk_fmt_state_json(json, sizeof json, &d, 0, 0, 0) > 0);
    cells = strstr(json, "\"cells\":[");
    CHECK(cells != NULL);
    EQ_I(count_char(cells, ','), 3);
    CHECK(strstr(cells, "null") == NULL);
}

// A truncated payload must be reported, never published: it would break every
// Home Assistant value_template reading it.
// The other half of the sign rule: charging must stay positive, and a resting
// pack at exactly 0 A must not come out as "-0.0".
static void test_fmt_state_json_power_sign(void)
{
    jk55_dynamic_t d;
    char json[1536];

    fill_dynamic(&d);
    d.current_ma = 2500;                 // charging
    d.power_mw   = 133120;
    CHECK(jk_fmt_state_json(json, sizeof json, &d, 0, 0, 0) > 0);
    CHECK(strstr(json, "\"power_w\":133.1") != NULL);
    CHECK(strstr(json, "\"power_w\":-") == NULL);

    fill_dynamic(&d);
    d.current_ma = 0;                    // at rest
    d.power_mw   = 0;
    CHECK(jk_fmt_state_json(json, sizeof json, &d, 0, 0, 0) > 0);
    CHECK(strstr(json, "\"power_w\":0.0") != NULL);
    CHECK(strstr(json, "\"power_w\":-0.0") == NULL);
}

static void test_fmt_state_json_truncation(void)
{
    jk55_dynamic_t d;
    fill_dynamic(&d);
    char small[64];
    EQ_I(jk_fmt_state_json(small, sizeof small, &d, 0, 0, 0), -1);
    EQ_U(strlen(small), sizeof small - 1);        // still NUL-terminated
    EQ_I(jk_fmt_state_json(NULL, 100, &d, 0, 0, 0), -1);
}

static void test_fmt_state_json_wild_indices(void)
{
    jk55_dynamic_t d;
    fill_dynamic(&d);
    d.cell_min_idx = 0xFF;            // corrupt, but sum8-valid
    d.cell_max_idx = 0xFE;
    char json[1536];
    CHECK(jk_fmt_state_json(json, sizeof json, &d, 0, 0, 0) > 0);
    CHECK(strstr(json, "\"cell_min_v\":0.000") != NULL);
}

static void test_fmt_settings_json(void)
{
    jk55_settings_t s;
    memset(&s, 0, sizeof s);
    s.cell_count = 16;
    s.design_capacity_mah = 280000;
    s.cell_ovp_mv = 3650;
    s.cell_uvp_mv = 2500;
    s.charge_current_ma = 200000;
    s.charge_enabled = true;

    char json[640];
    const int w = jk_fmt_settings_json(json, sizeof json, &s);
    CHECK(w > 0);
    EQ_I(w, (int)strlen(json));
    CHECK(strstr(json, "\"cell_count\":16") != NULL);
    CHECK(strstr(json, "\"design_ah\":280.000") != NULL);
    CHECK(strstr(json, "\"cell_ovp_v\":3.650") != NULL);
    CHECK(strstr(json, "\"charge_enabled\":true") != NULL);

    char small[32];
    EQ_I(jk_fmt_settings_json(small, sizeof small, &s), -1);
}

int main(void)
{
    test_sb_basic();
    test_sb_truncation_clamps();
    test_sb_exact_fit();
    test_sb_degenerate();

    test_demux_req();
    test_demux_jk55();
    test_demux_partial_jk55_not_given_to_req();
    test_demux_stalled_jk55_resyncs();
    test_demux_noise();
    test_demux_short();
    test_demux_mixed_stream();

    test_diag_silent();
    test_diag_noise_swapped_pair();
    test_diag_unframed();
    test_diag_few_errors_is_not_noise();
    test_diag_ok_needs_real_frames();
    test_diag_requests_without_answers();
    test_diag_overrun();
    test_diag_advice_always_present();

    test_fmt_cell_rows();
    test_fmt_cell_row();
    test_fmt_hex();
    test_fmt_cell_at_bounds();
    test_fmt_state_json();
    test_fmt_state_json_cell_holes();
    test_fmt_state_json_power_sign();
    test_fmt_state_json_truncation();
    test_fmt_state_json_wild_indices();
    test_fmt_settings_json();
    return test_report("util");
}
