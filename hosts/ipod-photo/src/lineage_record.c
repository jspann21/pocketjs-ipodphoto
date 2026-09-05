#include "storage.h"
#include "core_bridge.h"

#include <stdint.h>

#define LINEAGE_VERSION 2u
#define LINEAGE_COMMITTED 0x434f4d54u
#define LINEAGE_CRC_OFFSET (PJS_STORAGE_SECTOR_BYTES - 4u)

static const uint8_t lineage_magic[8] = {'P','J','S','L','I','F','E','2'};

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

void pjs_lineage_record_build(uint8_t record[PJS_STORAGE_SECTOR_BYTES],
                              const PjsLineageRecord *lineage,
                              bool committed)
{
    for (uint32_t index = 0u; index < PJS_STORAGE_SECTOR_BYTES; ++index) {
        record[index] = 0u;
    }
    for (uint32_t index = 0u; index < sizeof(lineage_magic); ++index) {
        record[index] = lineage_magic[index];
    }
    put32(record + 8u, LINEAGE_VERSION);
    put32(record + 12u, lineage->generation);
    put32(record + 16u, committed ? LINEAGE_COMMITTED : 0u);
    put32(record + 20u, lineage->phase);
    put32(record + 24u, lineage->active_source);
    put32(record + 28u, lineage->active_hash_low);
    put32(record + 32u, lineage->active_hash_high);
    put32(record + 36u, lineage->last_good_source);
    put32(record + 40u, lineage->last_good_hash_low);
    put32(record + 44u, lineage->last_good_hash_high);
    put32(record + 48u, lineage->trial_source);
    put32(record + 52u, lineage->trial_hash_low);
    put32(record + 56u, lineage->trial_hash_high);
    put32(record + 60u, lineage->rejected_source);
    put32(record + 64u, lineage->rejected_hash_low);
    put32(record + 68u, lineage->rejected_hash_high);
    put32(record + 72u, lineage->failure_stage);
    put32(record + 76u, lineage->failure_code);
    put32(record + LINEAGE_CRC_OFFSET, crc32(record, LINEAGE_CRC_OFFSET));
}

bool pjs_lineage_record_read(const uint8_t record[PJS_STORAGE_SECTOR_BYTES],
                             PjsLineageRecord *lineage_out)
{
    if (record == 0 || lineage_out == 0) return false;
    for (uint32_t index = 0u; index < sizeof(lineage_magic); ++index) {
        if (record[index] != lineage_magic[index]) return false;
    }
    if (get32(record + 8u) != LINEAGE_VERSION ||
        get32(record + 16u) != LINEAGE_COMMITTED ||
        get32(record + LINEAGE_CRC_OFFSET) != crc32(record, LINEAGE_CRC_OFFSET)) {
        return false;
    }
    PjsLineageRecord result = {
        .generation = get32(record + 12u),
        .phase = get32(record + 20u),
        .active_source = get32(record + 24u),
        .active_hash_low = get32(record + 28u),
        .active_hash_high = get32(record + 32u),
        .last_good_source = get32(record + 36u),
        .last_good_hash_low = get32(record + 40u),
        .last_good_hash_high = get32(record + 44u),
        .trial_source = get32(record + 48u),
        .trial_hash_low = get32(record + 52u),
        .trial_hash_high = get32(record + 56u),
        .rejected_source = get32(record + 60u),
        .rejected_hash_low = get32(record + 64u),
        .rejected_hash_high = get32(record + 68u),
        .failure_stage = get32(record + 72u),
        .failure_code = get32(record + 76u),
    };
    if (result.phase < PJS_LINEAGE_PHASE_ACTIVE ||
        result.phase > PJS_LINEAGE_PHASE_QUEUED ||
        result.active_source > PJS_BOOT_SOURCE_PACKAGE_BANK ||
        result.last_good_source > PJS_BOOT_SOURCE_PACKAGE_BANK ||
        result.trial_source > PJS_BOOT_SOURCE_PACKAGE_BANK ||
        result.rejected_source > PJS_BOOT_SOURCE_PACKAGE_BANK) return false;
    if (result.phase == PJS_LINEAGE_PHASE_QUEUED &&
        (result.trial_source != PJS_BOOT_SOURCE_PACKAGE_BANK ||
         (result.trial_hash_low == 0u && result.trial_hash_high == 0u) ||
         result.failure_stage != 0u || result.failure_code != 0u)) return false;
    *lineage_out = result;
    return true;
}
