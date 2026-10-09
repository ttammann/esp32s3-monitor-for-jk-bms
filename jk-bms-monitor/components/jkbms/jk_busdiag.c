#include "jk_busdiag.h"

// Above this share of errors-to-bytes, the line is not carrying data we can
// read at this baud. Framing errors are normal in ones and twos on a bus that
// starts mid-frame; they are diagnostic only when they dominate.
#define NOISE_NUMER 1
#define NOISE_DENOM 20   // 5%

jk_bus_verdict_t jk_bus_diagnose(const jk_busstats_t *s)
{
    const uint32_t errs = s->frame_err + s->parity_err + s->breaks;
    const uint32_t lost = s->fifo_ovf + s->buf_full;

    // Actual battery data beats everything: the link works, whatever else is
    // being counted.
    if (s->frames_jk55 > 0) {
        return JK_BUS_OK;
    }
    if (lost > 0) {
        // Losing bytes means the counts below cannot be trusted, so say that
        // rather than diagnosing from a sample with holes in it.
        return JK_BUS_OVERRUN;
    }
    // Requests without answers is its own fault, and a specific one: the pair
    // is wired, the baud is right and the BMS is awake and scanning. Counting
    // requests as "frames" would report this as a healthy link, which is how
    // the previous version of this function got it wrong.
    if (s->frames_req > 0) {
        return JK_BUS_NO_DATA;
    }
    if (s->bytes == 0) {
        // Errors with no bytes is still line activity: something is driving
        // the pair, we just cannot read a single character from it.
        return errs > 0 ? JK_BUS_NOISE : JK_BUS_SILENT;
    }
    if (errs * NOISE_DENOM > s->bytes * NOISE_NUMER) {
        return JK_BUS_NOISE;
    }
    return JK_BUS_UNFRAMED;
}

const char *jk_bus_advice(jk_bus_verdict_t v)
{
    switch (v) {
    case JK_BUS_OK:
        return "link OK";
    case JK_BUS_SILENT:
        return "nothing on the pair. Check: DIP at 0000 (any other address "
               "makes the BMS answer-only, so it stays silent), the RS485-P "
               "jack rather than the one next to CAN, both wires actually "
               "biting in the terminals, and the BMS awake";
    case JK_BUS_NOISE:
        return "line is active but unreadable. Most likely A and B swapped, "
               "or the wrong baud - try swapping the pair first, it costs "
               "nothing";
    case JK_BUS_UNFRAMED:
        return "clean bytes, nothing parses. Baud is probably right; the "
               "protocol is not one we decode. Capture the hex and look at "
               "the first bytes";
    case JK_BUS_OVERRUN:
        return "dropping data - RX buffer or FIFO overflowed. Counts above "
               "are unreliable";
    case JK_BUS_NO_DATA:
        return "bus is healthy and the BMS is scanning, but no pack answered. "
               "The wiring and baud are fine; the pack is not responding - "
               "check it is powered and on the RS485-P bus, not a different "
               "port";
    }
    return "unknown";
}
