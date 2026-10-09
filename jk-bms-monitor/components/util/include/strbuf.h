// Bounded string building.
//
// snprintf returns the length it *would* have written, so the natural-looking
// `w += snprintf(buf + w, n - w, ...)` walks the cursor past the end of the
// buffer as soon as anything truncates: the next call then gets an
// out-of-range pointer and an underflowed size_t. That exact bug appeared
// independently in the state-JSON builder and the HA discovery builder, which
// is reason enough for it to exist in one place with one test.
//
// Usage: start at 0, thread the return value through, check for overflow once
// at the end.
//
//     int w = sb_addf(buf, sizeof buf, 0, "{\"a\":%d,", a);
//     w = sb_addf(buf, sizeof buf, w, "\"b\":%d}", b);
//     if (sb_overflowed(sizeof buf, w)) { ...don't use buf... }
//
// Header-only, no ESP-IDF dependencies, so host tests cover it directly.
#pragma once

#include <stdio.h>
#include <stdarg.h>
#include <stddef.h>
#include <stdbool.h>

// Appends at offset w. Returns the new offset, clamped to n to mean "full".
// Once full, later calls write nothing and keep returning n, so a caller can
// build the whole string and test for truncation once at the end. The buffer
// is always left NUL-terminated (vsnprintf guarantees it).
__attribute__((format(printf, 4, 5)))
static inline int sb_addf(char *buf, size_t n, int w, const char *fmt, ...)
{
    if (buf == NULL || n == 0 || w < 0 || (size_t)w >= n) {
        return (int)n;
    }
    va_list ap;
    va_start(ap, fmt);
    const int r = vsnprintf(buf + w, n - (size_t)w, fmt, ap);
    va_end(ap);
    if (r < 0) {
        return (int)n;              // encoding error: treat as full
    }
    const long long nw = (long long)w + r;
    return (nw > (long long)n) ? (int)n : (int)nw;
}

// True if the string did not fit. Note the boundary: a result of exactly n
// means vsnprintf had to drop at least the NUL, so it is truncation too.
static inline bool sb_overflowed(size_t n, int w)
{
    return w < 0 || (size_t)w >= n;
}
