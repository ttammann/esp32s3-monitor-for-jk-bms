#include "jk_req.h"
#include "jk_crc.h"

int jk_req_len(const uint8_t *b, size_t n, uint16_t *reg_out, uint8_t *pack_out)
{
    if (n < 2) {
        return 0;
    }
    // Address first: it is the cheapest rejection and the one that does the
    // most work, since only 16 of 256 byte values can start a request.
    if (b[0] > JK_REQ_MAX_PACK) {
        return -1;
    }
    const uint8_t fn = b[1];
    if (fn != 0x06 && fn != 0x10) {
        return -1;
    }
    if (n < 4) {
        return 0;
    }
    const uint16_t reg = (uint16_t)((b[2] << 8) | b[3]);
    if (reg != JK_REQ_REG_SETTINGS && reg != JK_REQ_REG_DYNAMIC &&
        reg != JK_REQ_REG_STATIC) {
        return -1;
    }
    if (n < 8) {
        return 0;
    }

    // A 0x06 write and the 0x10 echo are both 8 bytes. Try that first: an
    // 11-byte 0x10 request whose first 8 bytes also CRC-match is a 1-in-65536
    // coincidence, and treating the echo as the common case keeps the stream
    // moving.
    if (jk_crc16_ok(b, 8)) {
        if (reg_out)  { *reg_out = reg; }
        if (pack_out) { *pack_out = b[0]; }
        return 8;
    }

    if (fn == 0x10) {
        const size_t rl = 9 + (size_t)b[6];
        if (rl > JK_REQ_MAX_LEN) {
            return -1;
        }
        if (n < rl) {
            return 0;
        }
        if (jk_crc16_ok(b, rl)) {
            if (reg_out)  { *reg_out = reg; }
            if (pack_out) { *pack_out = b[0]; }
            return (int)rl;
        }
    }
    return -1;
}
