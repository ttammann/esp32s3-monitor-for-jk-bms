// JK's "send me a frame" register writes -- the only non-55AA traffic on this
// bus.
//
// The BMS masters its own RS485-P bus. Once per scan cycle it writes 0x161E
// (settings) and 0x1620 (dynamic) to each of the sixteen pack addresses in
// turn; the addressed pack answers with a 55AA frame. Measured on this unit
// 2026-08-31: 559 of each per 220 s, i.e. 16 addresses x 2 registers x 35
// cycles, with zero dropped bytes.
//
// This recognises exactly those two writes. It replaced a general Modbus RTU
// framer, and the reason is worth recording: that framer brute-forced a CRC
// match across every length from 4 to 256 for any function code it did not
// model. On noise that fabricated a "valid" frame every ~588 bytes -- enough to
// make jk_bus_diagnose() report a healthy link on a dead bus, and long enough
// on occasion to swallow the start of a real 55AA frame. Matching a closed set
// of two registers cannot do either.
//
// No ESP-IDF dependencies, so it is host-testable.
#pragma once

#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>

#define JK_REQ_REG_SETTINGS  0x161E
#define JK_REQ_REG_DYNAMIC   0x1620
// A third register the vendor doc and the reference Python implementation both
// list. This BMS never issues it -- 220 s of capture contained 559 of each of
// the two above and none of this. Recognised anyway so that a firmware update
// which starts sending it is consumed cleanly rather than shed a byte at a
// time; no frame decoder is defined for whatever it would provoke, because
// nothing here has ever seen one.
#define JK_REQ_REG_STATIC    0x161C

// The BMS scans addresses 0x00..0x0F. Anything outside that cannot be a
// request, which is the bound that keeps noise from being read as one.
#define JK_REQ_MAX_PACK      0x0F

// A write-multiple request is 9 bytes plus its payload. Real ones on this bus
// are 11 (one register); the cap is slack, not a measurement.
#define JK_REQ_MAX_LEN       64

// Identify a frame request starting at buf[0]. Same contract as
// jk55_frame_len:
//   > 0  length in bytes, CRC verified
//     0  might still become one, feed more bytes
//   < 0  not a frame request, drop a byte and resync
// `reg_out` and `pack_out` may be NULL; they are only set on a positive return.
int jk_req_len(const uint8_t *buf, size_t len, uint16_t *reg_out,
               uint8_t *pack_out);
