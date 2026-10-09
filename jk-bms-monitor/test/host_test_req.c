// Host tests for the CRC and the frame-request recogniser.
//
// The recogniser replaced a general Modbus RTU framer, and the reason it had to
// be replaced is the last test in this file: the old framer brute-forced a CRC
// match across every length from 4 to 256 for any function code it did not
// model, which fabricated a "valid" frame roughly every 588 bytes of noise.
// That was enough to make the bus diagnosis report a healthy link on a dead
// bus, and occasionally long enough to swallow the start of a real 55AA frame.
#include <stdio.h>
#include <string.h>
#include "jk_crc.h"
#include "jk_req.h"
#include "test_util.h"

static size_t seal(uint8_t *b, size_t n)          // append the CRC
{
    const uint16_t c = jk_crc16(b, n);
    b[n]     = (uint8_t)c;
    b[n + 1] = (uint8_t)(c >> 8);
    return n + 2;
}

// The 8-byte form: a 0x10 echo, or a 0x06 write.
static size_t mk_req8(uint8_t *b, uint8_t pack, uint8_t fn, uint16_t reg)
{
    b[0] = pack; b[1] = fn;
    b[2] = (uint8_t)(reg >> 8); b[3] = (uint8_t)reg;
    b[4] = 0x00; b[5] = 0x01;
    return seal(b, 6);
}

// The 11-byte form: a 0x10 write-multiple carrying one register.
static size_t mk_req11(uint8_t *b, uint8_t pack, uint16_t reg)
{
    b[0] = pack; b[1] = 0x10;
    b[2] = (uint8_t)(reg >> 8); b[3] = (uint8_t)reg;
    b[4] = 0x00; b[5] = 0x01;
    b[6] = 0x02;                       // byte count
    b[7] = 0x00; b[8] = 0x00;          // payload
    return seal(b, 9);
}

static void test_crc_known_vector(void)
{
    EQ_U(jk_crc16((const uint8_t *)"123456789", 9), 0x4B37);
    EQ_U(jk_crc16((const uint8_t *)"", 0), 0xFFFF);     // seed, empty input
}

static void test_crc_ok_helper(void)
{
    uint8_t f[8];
    const size_t n = mk_req8(f, 0x00, 0x10, JK_REQ_REG_DYNAMIC);
    EQ_U(n, 8);
    CHECK(jk_crc16_ok(f, n));
    f[7] ^= 0xFF;
    CHECK(!jk_crc16_ok(f, n));
    CHECK(!jk_crc16_ok(f, 2));          // too short to hold a CRC
}

static void test_req_accepts_both_registers(void)
{
    uint8_t f[16];
    uint16_t reg = 0;
    uint8_t pack = 0xEE;

    EQ_I(jk_req_len(f, mk_req8(f, 0x00, 0x10, JK_REQ_REG_DYNAMIC), &reg, &pack), 8);
    EQ_U(reg, JK_REQ_REG_DYNAMIC);
    EQ_U(pack, 0x00);

    EQ_I(jk_req_len(f, mk_req8(f, 0x0F, 0x10, JK_REQ_REG_SETTINGS), &reg, &pack), 8);
    EQ_U(reg, JK_REQ_REG_SETTINGS);
    EQ_U(pack, 0x0F);

    // 0x06 write-single is the same length.
    EQ_I(jk_req_len(f, mk_req8(f, 0x03, 0x06, JK_REQ_REG_DYNAMIC), NULL, NULL), 8);

    // 0x161C is never seen on this bus but is consumed rather than shed a byte
    // at a time, in case a firmware update starts sending it.
    EQ_I(jk_req_len(f, mk_req8(f, 0x00, 0x10, JK_REQ_REG_STATIC), &reg, NULL), 8);
    EQ_U(reg, JK_REQ_REG_STATIC);
}

// The measured average on this bus was ~10.8 bytes per request, i.e. mostly
// this form.
static void test_req_11_byte_form(void)
{
    uint8_t f[16];
    uint16_t reg = 0;
    const size_t n = mk_req11(f, 0x00, JK_REQ_REG_SETTINGS);
    EQ_U(n, 11);
    EQ_I(jk_req_len(f, n, &reg, NULL), 11);
    EQ_U(reg, JK_REQ_REG_SETTINGS);
}

static void test_req_partial_asks_for_more(void)
{
    uint8_t f[16];
    const size_t n = mk_req11(f, 0x00, JK_REQ_REG_DYNAMIC);
    for (size_t k = 0; k < n; k++) {
        const int r = jk_req_len(f, k, NULL, NULL);
        // Never guess a length from a short buffer.
        CHECK(r == 0);
    }
    EQ_I(jk_req_len(f, n, NULL, NULL), 11);
}

static void test_req_rejects(void)
{
    uint8_t f[16];

    // Pack address outside 0x00..0x0F.
    mk_req8(f, 0x10, 0x10, JK_REQ_REG_DYNAMIC);
    EQ_I(jk_req_len(f, 8, NULL, NULL), -1);

    // Function code we do not model.
    mk_req8(f, 0x00, 0x03, JK_REQ_REG_DYNAMIC);
    EQ_I(jk_req_len(f, 8, NULL, NULL), -1);

    // A register that is not one of the two frame requests.
    mk_req8(f, 0x00, 0x10, 0x1234);
    EQ_I(jk_req_len(f, 8, NULL, NULL), -1);

    // A 0x06 write is only ever 8 bytes, so a bad CRC settles it immediately.
    mk_req8(f, 0x00, 0x06, JK_REQ_REG_DYNAMIC);
    f[7] = (uint8_t)~f[7];
    EQ_I(jk_req_len(f, 8, NULL, NULL), -1);
}

// A 0x10 with a bad 8-byte CRC is genuinely ambiguous: it could be the first
// eight bytes of an 11-byte request. The recogniser must ask for more rather
// than guess, and only reject once it has the whole claimed length.
static void test_req_ambiguous_then_rejected(void)
{
    uint8_t f[16];
    mk_req8(f, 0x00, 0x10, JK_REQ_REG_DYNAMIC);
    f[6] = 0x02;                              // claim a 2-byte payload => 11
    const uint16_t c8 = jk_crc16(f, 6);
    f[7] = (uint8_t)~(uint8_t)(c8 >> 8);      // guarantee the 8-byte CRC fails
    EQ_I(jk_req_len(f, 8, NULL, NULL), 0);
    EQ_I(jk_req_len(f, 10, NULL, NULL), 0);

    const uint16_t c11 = jk_crc16(f, 9);
    f[9]  = (uint8_t)~(uint8_t)c11;           // and the 11-byte CRC too
    f[10] = (uint8_t)~(uint8_t)(c11 >> 8);
    EQ_I(jk_req_len(f, 11, NULL, NULL), -1);

    // A claimed length past the cap is rejected outright rather than waited on,
    // so a garbage byte count cannot stall the stream for 264 bytes.
    mk_req8(f, 0x00, 0x10, JK_REQ_REG_DYNAMIC);
    f[6] = 0xFF;                              // 9 + 255 > JK_REQ_MAX_LEN
    const uint16_t c = jk_crc16(f, 6);
    f[7] = (uint8_t)~(uint8_t)(c >> 8);
    EQ_I(jk_req_len(f, 8, NULL, NULL), -1);
}

// A deliberately weak PRNG: reproducibility matters more than quality, and the
// point is volume.
static uint32_t rnd_state = 0x12345678u;
static uint8_t rnd_byte(void)
{
    rnd_state = rnd_state * 1664525u + 1013904223u;
    return (uint8_t)(rnd_state >> 24);
}

// The regression that motivated the rewrite. Uniform noise must never be
// recognised as a request. The old brute-force framer scored roughly one
// fabricated frame per 588 bytes here.
static void test_req_never_matches_noise(void)
{
    uint8_t b[64];
    int false_positives = 0;
    for (int trial = 0; trial < 200000; trial++) {
        for (size_t i = 0; i < sizeof b; i++) {
            b[i] = rnd_byte();
        }
        if (jk_req_len(b, sizeof b, NULL, NULL) > 0) {
            false_positives++;
        }
    }
    EQ_I(false_positives, 0);
}

int main(void)
{
    test_crc_known_vector();
    test_crc_ok_helper();
    test_req_accepts_both_registers();
    test_req_11_byte_form();
    test_req_partial_asks_for_more();
    test_req_rejects();
    test_req_ambiguous_then_rejected();
    test_req_never_matches_noise();
    return test_report("req");
}
