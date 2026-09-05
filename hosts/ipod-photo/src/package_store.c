#include "package_store.h"

#include <stddef.h>

#define HEADER_CRC_OFFSET 508u
#define HEADER_RESERVED_OFFSET 32u
#define HEADER_RESERVED_END HEADER_CRC_OFFSET

static uint32_t get32(const uint8_t *bytes)
{
    return (uint32_t)bytes[0] | ((uint32_t)bytes[1] << 8) |
           ((uint32_t)bytes[2] << 16) | ((uint32_t)bytes[3] << 24);
}

static void put32(uint8_t *bytes, uint32_t value)
{
    bytes[0] = (uint8_t)value;
    bytes[1] = (uint8_t)(value >> 8);
    bytes[2] = (uint8_t)(value >> 16);
    bytes[3] = (uint8_t)(value >> 24);
}

static uint32_t crc32_update(uint32_t crc, const uint8_t *bytes, uint32_t length)
{
    for (uint32_t index = 0u; index < length; ++index) {
        crc ^= bytes[index];
        for (uint32_t bit = 0u; bit < 8u; ++bit)
            crc = (crc >> 1) ^ (0xedb88320u & (uint32_t)-(int32_t)(crc & 1u));
    }
    return crc;
}

static uint32_t crc32(const uint8_t *bytes, uint32_t length)
{
    return ~crc32_update(0xffffffffu, bytes, length);
}

static bool zeroes(const uint8_t *bytes, uint32_t start, uint32_t end)
{
    for (uint32_t index = start; index < end; ++index)
        if (bytes[index] != 0u) return false;
    return true;
}

static bool equal(const uint8_t *left, const uint8_t *right)
{
    for (uint32_t index = 0u; index < PJS_PACKAGE_STORE_SECTOR_BYTES; ++index)
        if (left[index] != right[index]) return false;
    return true;
}

static void zero_output(uint8_t *destination, uint32_t capacity,
                        PjsPackageStoreHeader *out)
{
    if (destination != 0) {
        for (uint32_t index = 0u; index < capacity; ++index)
            destination[index] = 0u;
    }
    if (out != 0) *out = (PjsPackageStoreHeader){0};
}

static int decode_header(const uint8_t bytes[PJS_PACKAGE_STORE_SECTOR_BYTES],
                         PjsPackageStoreHeader *out)
{
    if (get32(bytes) != PJS_PACKAGE_STORE_HEADER_MAGIC ||
        get32(bytes + 4u) != PJS_PACKAGE_STORE_HEADER_VERSION ||
        get32(bytes + 28u) != PJS_PACKAGE_STORE_HEADER_COMMITTED ||
        !zeroes(bytes, HEADER_RESERVED_OFFSET, HEADER_RESERVED_END) ||
        get32(bytes + HEADER_CRC_OFFSET) != crc32(bytes, HEADER_CRC_OFFSET)) {
        return PJS_PACKAGE_STORE_ERR_FORMAT;
    }
    uint32_t length = get32(bytes + 12u);
    if (length == 0u || length > PJS_PACKAGE_STORE_PAYLOAD_LIMIT)
        return PJS_PACKAGE_STORE_ERR_FORMAT;
    if (out != 0) {
        *out = (PjsPackageStoreHeader){
            .generation = get32(bytes + 8u),
            .payload_length = length,
            .payload_crc = get32(bytes + 16u),
            .hash_low = get32(bytes + 20u),
            .hash_high = get32(bytes + 24u),
        };
    }
    return PJS_PACKAGE_STORE_OK;
}

int pjs_package_store_inspect_header(const PjsPackageStoreIo *io,
                                     uint32_t bank,
                                     PjsPackageStoreHeader *out)
{
    uint8_t header[PJS_PACKAGE_STORE_SECTOR_BYTES];
    if (out != 0) *out = (PjsPackageStoreHeader){0};
    if (io == 0 || io->read == 0 || out == 0 ||
        bank >= PJS_PACKAGE_STORE_BANK_COUNT)
        return PJS_PACKAGE_STORE_ERR_ARGUMENT;
    if (!io->read(io->context, bank, 0u, header))
        return PJS_PACKAGE_STORE_ERR_IO;
    return decode_header(header, out);
}

int pjs_package_store_load_exact(const PjsPackageStoreIo *io, uint32_t bank,
                                 uint32_t expected_hash_low,
                                 uint32_t expected_hash_high,
                                 uint8_t *destination,
                                 uint32_t destination_capacity,
                                 PjsPackageStoreHeader *out)
{
    uint8_t header_bytes[PJS_PACKAGE_STORE_SECTOR_BYTES];
    PjsPackageStoreHeader header = {0};
    zero_output(destination, destination_capacity, out);
    if (io == 0 || io->read == 0 || destination == 0 ||
        bank >= PJS_PACKAGE_STORE_BANK_COUNT)
        return PJS_PACKAGE_STORE_ERR_ARGUMENT;
    if (!io->read(io->context, bank, 0u, header_bytes))
        return PJS_PACKAGE_STORE_ERR_IO;
    int result = decode_header(header_bytes, &header);
    if (result != PJS_PACKAGE_STORE_OK) return result;
    if (header.hash_low != expected_hash_low ||
        header.hash_high != expected_hash_high)
        return PJS_PACKAGE_STORE_ERR_HASH;
    if (destination_capacity < header.payload_length)
        return PJS_PACKAGE_STORE_ERR_CAPACITY;

    uint8_t sector[PJS_PACKAGE_STORE_SECTOR_BYTES];
    uint32_t remaining = header.payload_length;
    uint32_t offset = 0u;
    uint32_t payload_crc = 0xffffffffu;
    for (uint32_t sector_index = 0u; remaining != 0u; ++sector_index) {
        uint32_t count = remaining > PJS_PACKAGE_STORE_SECTOR_BYTES ?
            PJS_PACKAGE_STORE_SECTOR_BYTES : remaining;
        if (!io->read(io->context, bank, sector_index + 1u, sector)) {
            zero_output(destination, destination_capacity, out);
            return PJS_PACKAGE_STORE_ERR_IO;
        }
        for (uint32_t index = 0u; index < count; ++index)
            destination[offset + index] = sector[index];
        payload_crc = crc32_update(payload_crc, sector, count);
        remaining -= count;
        offset += count;
    }
    if ((~payload_crc) != header.payload_crc) {
        zero_output(destination, destination_capacity, out);
        return PJS_PACKAGE_STORE_ERR_CRC;
    }
    if (out != 0) *out = header;
    return PJS_PACKAGE_STORE_OK;
}

static int invalidate(const PjsPackageStoreIo *io, uint32_t bank,
                      uint8_t header[PJS_PACKAGE_STORE_SECTOR_BYTES])
{
    for (uint32_t index = 0u; index < PJS_PACKAGE_STORE_SECTOR_BYTES; ++index)
        header[index] = 0u;
    if (!io->write(io->context, bank, 0u, header) ||
        !io->flush(io->context, bank) ||
        !io->read(io->context, bank, 0u, header) ||
        !zeroes(header, 0u, PJS_PACKAGE_STORE_SECTOR_BYTES))
        return PJS_PACKAGE_STORE_ERR_IO;
    return PJS_PACKAGE_STORE_OK;
}

int pjs_package_store_stage(const PjsPackageStoreIo *io, uint32_t bank,
                            uint32_t generation, uint32_t expected_payload_crc,
                            uint32_t hash_low, uint32_t hash_high,
                            const uint8_t *payload, uint32_t payload_length)
{
    uint8_t sector[PJS_PACKAGE_STORE_SECTOR_BYTES];
    uint8_t header[PJS_PACKAGE_STORE_SECTOR_BYTES];
    if (io == 0 || io->read == 0 || io->write == 0 || io->flush == 0 ||
        payload == 0 || payload_length == 0u ||
        payload_length > PJS_PACKAGE_STORE_PAYLOAD_LIMIT ||
        bank >= PJS_PACKAGE_STORE_BANK_COUNT)
        return PJS_PACKAGE_STORE_ERR_ARGUMENT;
    uint32_t payload_crc = crc32(payload, payload_length);
    if (payload_crc != expected_payload_crc) return PJS_PACKAGE_STORE_ERR_CRC;
    int result = invalidate(io, bank, header);
    if (result != PJS_PACKAGE_STORE_OK) return result;

    uint32_t sectors = (payload_length + PJS_PACKAGE_STORE_SECTOR_BYTES - 1u) /
                       PJS_PACKAGE_STORE_SECTOR_BYTES;
    uint32_t offset = 0u;
    for (uint32_t sector_index = 0u; sector_index < sectors; ++sector_index) {
        uint32_t remaining = payload_length - offset;
        uint32_t count = remaining > PJS_PACKAGE_STORE_SECTOR_BYTES ?
            PJS_PACKAGE_STORE_SECTOR_BYTES : remaining;
        for (uint32_t index = 0u; index < PJS_PACKAGE_STORE_SECTOR_BYTES; ++index)
            sector[index] = index < count ? payload[offset + index] : 0u;
        if (!io->write(io->context, bank, sector_index + 1u, sector))
            return PJS_PACKAGE_STORE_ERR_IO;
        offset += count;
    }
    if (!io->flush(io->context, bank)) return PJS_PACKAGE_STORE_ERR_IO;

    offset = 0u;
    for (uint32_t sector_index = 0u; sector_index < sectors; ++sector_index) {
        uint32_t remaining = payload_length - offset;
        uint32_t count = remaining > PJS_PACKAGE_STORE_SECTOR_BYTES ?
            PJS_PACKAGE_STORE_SECTOR_BYTES : remaining;
        if (!io->read(io->context, bank, sector_index + 1u, sector))
            return PJS_PACKAGE_STORE_ERR_IO;
        for (uint32_t index = 0u; index < PJS_PACKAGE_STORE_SECTOR_BYTES; ++index) {
            uint8_t expected = index < count ? payload[offset + index] : 0u;
            if (sector[index] != expected) return PJS_PACKAGE_STORE_ERR_IO;
        }
        offset += count;
    }

    for (uint32_t index = 0u; index < PJS_PACKAGE_STORE_SECTOR_BYTES; ++index)
        header[index] = 0u;
    put32(header, PJS_PACKAGE_STORE_HEADER_MAGIC);
    put32(header + 4u, PJS_PACKAGE_STORE_HEADER_VERSION);
    put32(header + 8u, generation);
    put32(header + 12u, payload_length);
    put32(header + 16u, payload_crc);
    put32(header + 20u, hash_low);
    put32(header + 24u, hash_high);
    put32(header + 28u, PJS_PACKAGE_STORE_HEADER_COMMITTED);
    put32(header + HEADER_CRC_OFFSET, crc32(header, HEADER_CRC_OFFSET));
    if (!io->write(io->context, bank, 0u, header) ||
        !io->flush(io->context, bank) ||
        !io->read(io->context, bank, 0u, sector) || !equal(header, sector))
        return PJS_PACKAGE_STORE_ERR_IO;
    return PJS_PACKAGE_STORE_OK;
}
