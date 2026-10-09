// Console and JSON formatting for the listener.
//
// This exists so the indexing is testable. It used to live inside the listener
// alongside the ESP-IDF log calls, where it could not be compiled on the host —
// which meant the one piece of code doing raw offset arithmetic over frame
// bytes was also the only piece with no tests. The log calls stay in the
// listener; the string building lives here.
//
// No ESP-IDF dependencies: values the host cannot know (RSSI, uptime) are
// passed in.
#pragma once

#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>
#include "jk55.h"

// How many 8-cell rows are worth printing, given the presence bitmap. Returns
// 0..4. A bitmap with a gap still prints the rows spanning it, because a hole
// is information: it means a cell the pack expects is missing.
int jk_fmt_cell_rows(uint32_t cell_present);

// Format one 8-cell row into `out`, e.g. "3273 3274 3274 3275 ...".
// `row` is 0-based and must be < jk_fmt_cell_rows(). Returns the number of
// characters written, or -1 if the row is out of range or it did not fit.
// `out` is always NUL-terminated when on > 0.
int jk_fmt_cell_row(char *out, size_t on, const uint16_t *cell_mv,
                    size_t ncells, int row);

// Bounded hex dump: "55 AA EB 90". Truncates rather than overflowing, and
// always NUL-terminates when on > 0.
void jk_fmt_hex(char *out, size_t on, const uint8_t *b, size_t n);

// Cell voltage at a wire-supplied index. The BMS sends cell_min_idx and
// cell_max_idx as raw bytes, so they are attacker-shaped as far as this code is
// concerned: a corrupt frame that passes sum8 could carry 0xFF. Returns 0 for
// an out-of-range index rather than reading past the array.
uint16_t jk_fmt_cell_at(const jk55_dynamic_t *d, uint8_t idx);

// Build the MQTT state payload. Returns the JSON length, or -1 if it did not
// fit — a truncated payload must never be published, because it breaks every
// Home Assistant value_template that reads it.
//
// There is deliberately no `comms_ok` field. One used to exist, and its only
// caller passed a literal `true`, so the "BMS comms" entity reading it could
// never once report a disconnection — the single thing it was for. Data
// freshness is published as a retained online/offline payload on the
// bms_status topic instead, which this function has no business knowing about:
// a state payload only exists when a frame just arrived, so any freshness flag
// computed here is true by construction.
int jk_fmt_state_json(char *buf, size_t n, const jk55_dynamic_t *d,
                      int rssi, int64_t uptime_s, uint32_t frame_count);

// Build the retained settings payload. Same contract.
int jk_fmt_settings_json(char *buf, size_t n, const jk55_settings_t *s);
