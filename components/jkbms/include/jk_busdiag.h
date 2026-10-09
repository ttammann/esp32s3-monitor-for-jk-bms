// Bus-level diagnosis for the 55AA listener.
//
// "No data" is several completely different faults wearing one message:
// nothing is electrically connected; something is transmitting but the UART
// cannot frame it (wrong baud, or A/B swapped); bytes arrive cleanly but belong
// to a protocol we do not parse; or the bus is healthy and the BMS is scanning
// but no pack answers. They need opposite fixes, so the listener counts what it
// actually saw and says which one it is.
//
// Pure, no ESP-IDF dependencies, host-tested — the verdict decides what the
// user does next, and sending them to rewire a correctly wired bus costs an
// evening.
#pragma once

#include <stdint.h>
#include <stdbool.h>

typedef struct {
    uint32_t bytes;          // bytes handed over by the UART driver
    uint32_t frames_req;     // frame-request writes (the BMS scanning)
    uint32_t frames_jk55;    // complete, checksum-valid 55AA frames
    uint32_t dropped;        // bytes shed while resyncing
    uint32_t frame_err;      // UART framing errors
    uint32_t parity_err;
    uint32_t breaks;         // line held low longer than a character
    uint32_t fifo_ovf;       // hardware FIFO overflowed
    uint32_t buf_full;       // driver ring buffer overflowed
} jk_busstats_t;

typedef enum {
    JK_BUS_SILENT,    // no bytes, no errors: nothing is driving the pair
    JK_BUS_NOISE,     // errors dominate: wrong baud, or A and B swapped
    JK_BUS_UNFRAMED,  // clean bytes, nothing parses: unknown protocol or baud
    JK_BUS_OVERRUN,   // losing data faster than we consume it
    JK_BUS_NO_DATA,   // BMS is scanning, but no pack answered
    JK_BUS_OK,        // 55AA frames are decoding
} jk_bus_verdict_t;

jk_bus_verdict_t jk_bus_diagnose(const jk_busstats_t *s);

// One line of plain guidance for a verdict. Never NULL.
const char *jk_bus_advice(jk_bus_verdict_t v);
