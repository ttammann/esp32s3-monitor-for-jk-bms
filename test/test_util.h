// Minimal check macros. Deliberately not assert(): assert aborts on the first
// failure, so one broken decode hides every later result, and it compiles out
// entirely under -DNDEBUG. These always run and report every failure.
#pragma once

#include <stdio.h>
#include <string.h>
#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>
#include <inttypes.h>

static int test_fails;
static int test_checks;

#define CHECK(cond) do {                                                  \
    test_checks++;                                                        \
    if (!(cond)) {                                                        \
        test_fails++;                                                     \
        printf("  FAIL %s:%d  %s\n", __FILE__, __LINE__, #cond);          \
    }                                                                     \
} while (0)

// Separate signed/unsigned forms so the failure message prints the values.
#define EQ_U(got, want) do {                                              \
    test_checks++;                                                        \
    uintmax_t g_ = (uintmax_t)(got), w_ = (uintmax_t)(want);              \
    if (g_ != w_) {                                                       \
        test_fails++;                                                     \
        printf("  FAIL %s:%d  %s: got %ju, want %ju\n",                   \
               __FILE__, __LINE__, #got, g_, w_);                         \
    }                                                                     \
} while (0)

#define EQ_I(got, want) do {                                              \
    test_checks++;                                                        \
    intmax_t g_ = (intmax_t)(got), w_ = (intmax_t)(want);                 \
    if (g_ != w_) {                                                       \
        test_fails++;                                                     \
        printf("  FAIL %s:%d  %s: got %jd, want %jd\n",                   \
               __FILE__, __LINE__, #got, g_, w_);                         \
    }                                                                     \
} while (0)

static inline int test_report(const char *name)
{
    printf("%-8s %d checks, %d failed%s\n",
           name, test_checks, test_fails, test_fails ? "  <-- FAILED" : "");
    return test_fails ? 1 : 0;
}

// --- shared stream harness -------------------------------------------------
//
// Three test files used to carry their own copy of this loop, and the copies had
// already drifted: two spelled the termination condition `have >= 4` and one
// `have > 0`, and two open-coded `fed == n` as a stall proxy while the third
// passed a real `stalled` flag. Since the loop's whole job is to reproduce the
// listener's drain sequence, three different loops meant three different things
// were being tested. One harness, parameterised by how the front of the buffer
// is classified.

typedef enum {
    TH_NEED_MORE = 0,   // stop draining, feed more bytes
    TH_ITEM,            // *len_out bytes consumed as a recognised item
    TH_DROP,            // drop one byte and resync
} th_action_t;

// Classify the front of `buf`. `kind_out` is an opaque tag the caller can use
// to count different item types; it is passed straight back in th_result_t.
typedef th_action_t (*th_classify_fn)(const uint8_t *buf, size_t n,
                                      bool stalled, size_t *len_out,
                                      int *kind_out, void *ctx);

#define TH_MAX_KINDS 4

typedef struct {
    int items;                  // total TH_ITEM results
    int kind[TH_MAX_KINDS];     // per-kind counts
    int dropped;                // bytes shed resyncing
} th_result_t;

// Feed `s` through `classify` in the given repeating chunk sizes, draining
// after each chunk exactly as the listener does.
static inline th_result_t th_run(const uint8_t *s, size_t n,
                                 const size_t *chunks, size_t nchunks,
                                 uint8_t *buf, size_t bufsz,
                                 th_classify_fn classify, void *ctx)
{
    th_result_t r;
    memset(&r, 0, sizeof r);
    size_t have = 0, fed = 0, ci = 0;

    while (fed < n || have > 0) {
        if (fed < n) {
            size_t c = chunks[ci++ % nchunks];
            if (c > n - fed)        { c = n - fed; }
            if (c > bufsz - have)   { c = bufsz - have; }
            memcpy(buf + have, s + fed, c);
            have += c; fed += c;
        }
        // The listener stalls on a full buffer or a gap in arrivals; once the
        // stream is exhausted every remaining byte is by definition stalled.
        const bool stalled = (have == bufsz) || (fed == n);
        bool draining = true;
        while (draining && have > 0) {
            size_t len = 0;
            int kind = 0;
            switch (classify(buf, have, stalled, &len, &kind, ctx)) {
            case TH_ITEM:
                r.items++;
                if (kind >= 0 && kind < TH_MAX_KINDS) { r.kind[kind]++; }
                break;
            case TH_DROP:
                len = 1;
                r.dropped++;
                break;
            case TH_NEED_MORE:
            default:
                draining = false;
                continue;
            }
            if (len == 0 || len > have) { return r; }   // classifier bug
            memmove(buf, buf + len, have - len);
            have -= len;
        }
        if (!draining && fed >= n) { break; }
    }
    return r;
}
