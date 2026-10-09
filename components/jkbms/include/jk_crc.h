// CRC-16/MODBUS: polynomial 0xA001, initial value 0xFFFF, low byte first.
//
// The name is the algorithm's, not a protocol dependency. Nothing in this
// firmware speaks Modbus; JK's 55AA stream simply borrows this checksum in two
// places -- the 308-byte frame variant, and the register writes the BMS uses to
// request a frame from each pack.
//
// No ESP-IDF dependencies, so it is host-testable.
#pragma once

#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>

uint16_t jk_crc16(const uint8_t *data, size_t len);

// True if the last two bytes of `buf` are a correct little-endian CRC over the
// preceding len-2 bytes. Returns false for len < 3.
bool jk_crc16_ok(const uint8_t *buf, size_t len);
