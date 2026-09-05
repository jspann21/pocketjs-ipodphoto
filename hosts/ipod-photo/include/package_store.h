#ifndef POCKETJS_IPOD_PHOTO_PACKAGE_STORE_H
#define POCKETJS_IPOD_PHOTO_PACKAGE_STORE_H

#include <stdbool.h>
#include <stdint.h>

#define PJS_PACKAGE_STORE_SECTOR_BYTES 512u
#define PJS_PACKAGE_STORE_BANK_COUNT 2u
#define PJS_PACKAGE_STORE_BANK_SECTORS 8192u
#define PJS_PACKAGE_STORE_PAYLOAD_LIMIT \
    ((PJS_PACKAGE_STORE_BANK_SECTORS - 1u) * \
     PJS_PACKAGE_STORE_SECTOR_BYTES)

#define PJS_PACKAGE_STORE_HEADER_MAGIC 0x50534a50u /* "PJSP" */
#define PJS_PACKAGE_STORE_HEADER_VERSION 1u
#define PJS_PACKAGE_STORE_HEADER_COMMITTED 0x4d4d4f43u /* "COMM" */

enum {
    PJS_PACKAGE_STORE_OK = 0,
    PJS_PACKAGE_STORE_ERR_ARGUMENT = -1,
    PJS_PACKAGE_STORE_ERR_IO = -2,
    PJS_PACKAGE_STORE_ERR_FORMAT = -3,
    PJS_PACKAGE_STORE_ERR_CRC = -4,
    PJS_PACKAGE_STORE_ERR_HASH = -5,
    PJS_PACKAGE_STORE_ERR_CAPACITY = -6,
};

typedef bool (*PjsPackageStoreRead)(void *context, uint32_t bank,
                                    uint32_t sector,
                                    uint8_t bytes[PJS_PACKAGE_STORE_SECTOR_BYTES]);
typedef bool (*PjsPackageStoreWrite)(void *context, uint32_t bank,
                                     uint32_t sector,
                                     const uint8_t bytes[PJS_PACKAGE_STORE_SECTOR_BYTES]);
typedef bool (*PjsPackageStoreFlush)(void *context, uint32_t bank);

typedef struct {
    void *context;
    PjsPackageStoreRead read;
    PjsPackageStoreWrite write;
    PjsPackageStoreFlush flush;
} PjsPackageStoreIo;

typedef struct {
    uint32_t generation;
    uint32_t payload_length;
    uint32_t payload_crc;
    uint32_t hash_low;
    uint32_t hash_high;
} PjsPackageStoreHeader;

/* Callers own physical bank mapping, lineage protection, and publication.
 * These callbacks address exactly one bank and sector; this module never
 * chooses the active/last-good bank and never writes lineage metadata. */
int pjs_package_store_inspect_header(const PjsPackageStoreIo *io,
                                     uint32_t bank,
                                     PjsPackageStoreHeader *out);

/* Loads only a fully committed record matching both expected hash words.
 * On failure, destination (when supplied) and out are zero-filled. The
 * caller must perform package-format admission after this exact load. */
int pjs_package_store_load_exact(const PjsPackageStoreIo *io, uint32_t bank,
                                 uint32_t expected_hash_low,
                                 uint32_t expected_hash_high,
                                 uint8_t *destination,
                                 uint32_t destination_capacity,
                                 PjsPackageStoreHeader *out);

/* Stages a complete payload into the explicitly selected bank. The payload
 * CRC is computed and compared with expected_payload_crc; hash words are
 * caller-supplied values produced by package admission. No lineage is
 * published and no bank is selected by this function. */
int pjs_package_store_stage(const PjsPackageStoreIo *io, uint32_t bank,
                            uint32_t generation, uint32_t expected_payload_crc,
                            uint32_t hash_low, uint32_t hash_high,
                            const uint8_t *payload, uint32_t payload_length);

#endif
