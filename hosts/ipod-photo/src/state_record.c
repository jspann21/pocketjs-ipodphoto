#include "storage.h"

#include <stdint.h>

#define STATE_VERSION 1u
#define STATE_COMMITTED 0x434f4d54u
#define STATE_CRC_OFFSET (PJS_STORAGE_SECTOR_BYTES - 4u)

static const uint8_t state_magic[8] = {'P','J','S','S','T','A','T','E'};

static void put32(uint8_t *bytes, uint32_t value)
{
    bytes[0] = (uint8_t)value;
    bytes[1] = (uint8_t)(value >> 8);
    bytes[2] = (uint8_t)(value >> 16);
    bytes[3] = (uint8_t)(value >> 24);
}

static uint32_t get32(const uint8_t *bytes)
{
    return (uint32_t)bytes[0] | ((uint32_t)bytes[1] << 8) |
           ((uint32_t)bytes[2] << 16) | ((uint32_t)bytes[3] << 24);
}

static uint32_t crc32(const uint8_t *bytes, uint32_t length)
{
    uint32_t crc = 0xffffffffu;
    for (uint32_t index = 0u; index < length; ++index) {
        crc ^= bytes[index];
        for (uint32_t bit = 0u; bit < 8u; ++bit) {
            uint32_t mask = 0u - (crc & 1u);
            crc = (crc >> 1) ^ (0xedb88320u & mask);
        }
    }
    return ~crc;
}

void pjs_state_record_build(uint8_t record[PJS_STORAGE_SECTOR_BYTES],
                            uint32_t generation, uint32_t payload,
                            bool committed)
{
    for (uint32_t index = 0u; index < PJS_STORAGE_SECTOR_BYTES; ++index) {
        record[index] = 0u;
    }
    for (uint32_t index = 0u; index < sizeof(state_magic); ++index) {
        record[index] = state_magic[index];
    }
    put32(record + 8u, STATE_VERSION);
    put32(record + 12u, generation);
    put32(record + 16u, payload);
    put32(record + 20u, committed ? STATE_COMMITTED : 0u);
    put32(record + STATE_CRC_OFFSET, crc32(record, STATE_CRC_OFFSET));
}

bool pjs_state_record_read(const uint8_t record[PJS_STORAGE_SECTOR_BYTES],
                           uint32_t *generation_out,
                           uint32_t *payload_out)
{
    if (record == 0 || generation_out == 0 || payload_out == 0) return false;
    for (uint32_t index = 0u; index < sizeof(state_magic); ++index) {
        if (record[index] != state_magic[index]) return false;
    }
    if (get32(record + 8u) != STATE_VERSION ||
        get32(record + 20u) != STATE_COMMITTED ||
        get32(record + STATE_CRC_OFFSET) != crc32(record, STATE_CRC_OFFSET)) {
        return false;
    }
    *generation_out = get32(record + 12u);
    *payload_out = get32(record + 16u);
    return true;
}
