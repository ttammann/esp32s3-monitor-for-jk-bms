#include "jk_crc.h"

uint16_t jk_crc16(const uint8_t *data, size_t len)
{
    uint16_t crc = 0xFFFF;
    for (size_t i = 0; i < len; i++) {
        crc ^= data[i];
        for (int b = 0; b < 8; b++) {
            crc = (crc & 1) ? (uint16_t)((crc >> 1) ^ 0xA001) : (uint16_t)(crc >> 1);
        }
    }
    return crc;
}

bool jk_crc16_ok(const uint8_t *buf, size_t len)
{
    if (len < 3) {
        return false;
    }
    const uint16_t c = jk_crc16(buf, len - 2);
    return buf[len - 2] == (uint8_t)c && buf[len - 1] == (uint8_t)(c >> 8);
}
