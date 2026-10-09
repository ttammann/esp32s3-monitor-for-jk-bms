// Protocol demultiplexer for the 55AA listener.
//
// The listener sees an undelimited byte stream carrying two things: JK's 55AA
// push frames, and the register writes the BMS uses to request them. Deciding
// what to do with the front of the buffer is the subtlest logic in the
// firmware, so it lives here as a pure function with no ESP-IDF dependencies
// and is host-tested directly.
#pragma once

#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>
#include "jk55.h"

typedef enum {
    JK_DEMUX_NEED_MORE = 0,   // wait for more bytes; nothing to do yet
    JK_DEMUX_REQ,             // a frame-request write (see jk_req.h)
    JK_DEMUX_JK55,            // a 55AA frame
    JK_DEMUX_DROP1,           // buf[0] cannot start either; drop it, resync
} jk_demux_action_t;

// Results for the item at the front of the buffer. Grouped into a struct
// because the two protocols report different things and threading five
// out-parameters through every call site was worse.
typedef struct {
    size_t       len;       // bytes at buf[0] belonging to this item
    jk55_check_t ck;        // JK55 only: which checksum validated it
    uint16_t     req_reg;   // REQ only: 0x161E or 0x1620
    uint8_t      req_pack;  // REQ only: which pack address was addressed
} jk_demux_out_t;

// Classify the front of `buf`.
//
// `stalled` must be true when no new bytes have arrived for longer than the
// resync timeout, or the buffer is full. It is what stops a truncated frame
// from wedging the stream forever: without it a valid header whose body never
// arrives would return NEED_MORE indefinitely.
//
// `out` may be NULL; it is fully zeroed on entry otherwise.
jk_demux_action_t jk_demux(const uint8_t *buf, size_t n, bool stalled,
                           jk_demux_out_t *out);
