#ifndef POCKETJS_IPOD_PHOTO_STORAGE_H
#define POCKETJS_IPOD_PHOTO_STORAGE_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "package_store.h"

#define PJS_STORAGE_SECTOR_BYTES 512u
#define PJS_STORAGE_MAX_FILE_SECTORS 136u
#define PJS_STORAGE_MAX_BOUNDED_FILE_SECTORS 8192u
#define PJS_STORAGE_MAX_FILE_BYTES (8u * 1024u * 1024u)
#define PJS_STORAGE_MAX_APPS 6u

#define PJS_STORAGE_OK 0
#define PJS_STORAGE_ERR_ARGUMENT -1
#define PJS_STORAGE_ERR_ATA -2
#define PJS_STORAGE_ERR_PARTITION -3
#define PJS_STORAGE_ERR_BPB -4
#define PJS_STORAGE_ERR_NOT_FOUND -5
#define PJS_STORAGE_ERR_CHAIN -6
#define PJS_STORAGE_ERR_TOO_LARGE -7
#define PJS_STORAGE_ERR_ALLOC -8
#define PJS_STORAGE_ERR_SHORT_READ -9
#define PJS_STORAGE_ERR_TOO_MANY -10
#define PJS_STORAGE_ERR_STATE -11
#define PJS_STORAGE_ERR_VERIFY -12
#define PJS_STORAGE_ERR_NOT_OWNED -13
#define PJS_STORAGE_ERR_BUSY -14
#define PJS_STORAGE_ERR_TIMEOUT -15

/* A handoff is intentionally shorter than the normal ATA command timeout:
 * the caller must retain control of the running firmware if the device does
 * not quiesce promptly. */
#define PJS_STORAGE_ATA_QUIESCE_TIMEOUT_US 250000u

enum {
    PJS_STORAGE_ATA_HANDOFF_NOT_OWNED = 0u,
    PJS_STORAGE_ATA_HANDOFF_READY = 1u,
    PJS_STORAGE_ATA_HANDOFF_BUSY = 2u,
    PJS_STORAGE_ATA_HANDOFF_DATA = 3u,
    PJS_STORAGE_ATA_HANDOFF_FAULT = 4u,
    PJS_STORAGE_ATA_HANDOFF_TIMEOUT = 5u,
    PJS_STORAGE_ATA_HANDOFF_UNKNOWN = 6u,
};

typedef struct {
    uint8_t state;
    uint8_t ata_status;
    uint16_t polls;
    uint32_t elapsed_us;
} PjsStorageDiskHandoff;

enum {
    PJS_STORAGE_DISK_UNKNOWN = 0u,
    PJS_STORAGE_DISK_ON = 1u,
    PJS_STORAGE_DISK_FLUSHED = 2u,
    PJS_STORAGE_DISK_STANDBY = 3u,
    PJS_STORAGE_DISK_OFF = 4u,
    PJS_STORAGE_DISK_NOT_OWNED = 5u,
    PJS_STORAGE_DISK_REFUSED = 6u,
};

typedef struct {
    uint8_t state;
    uint8_t ata_status;
    uint16_t commands;
    uint32_t elapsed_us;
} PjsStorageDiskPower;

typedef bool (*PjsSectorReadFn)(void *context, uint32_t lba,
                                uint8_t sector[PJS_STORAGE_SECTOR_BYTES]);

typedef struct {
    PjsSectorReadFn read_sector;
    void *context;
    uint32_t partition_lba;
    uint32_t partition_sectors;
    uint32_t fat_lba;
    uint32_t data_lba;
    uint32_t fat_sectors;
    uint32_t root_cluster;
    uint32_t cluster_count;
    uint8_t sectors_per_cluster;
    uint8_t fat_count;
} PjsFat32;

typedef struct {
    uint8_t *bytes;
    uint32_t length;
} PjsStorageFile;

typedef struct {
    char file_name[11];
    uint32_t size;
} PjsStorageApp;

typedef struct {
    PjsStorageApp apps[PJS_STORAGE_MAX_APPS];
    uint32_t count;
} PjsStorageCatalog;

typedef struct {
    uint32_t available;
    uint32_t active_slot;
    uint32_t generation;
    uint32_t payload;
    uint32_t error;
} PjsPersistenceState;

#define PJS_LINEAGE_PHASE_ACTIVE 1u
#define PJS_LINEAGE_PHASE_TRIAL 2u
#define PJS_LINEAGE_PHASE_RUNNING 3u
#define PJS_LINEAGE_PHASE_CRASHED 4u
#define PJS_LINEAGE_PHASE_QUEUED 5u

typedef struct {
    uint32_t generation;
    uint32_t phase;
    uint32_t active_source;
    uint32_t active_hash_low;
    uint32_t active_hash_high;
    uint32_t last_good_source;
    uint32_t last_good_hash_low;
    uint32_t last_good_hash_high;
    uint32_t trial_source;
    uint32_t trial_hash_low;
    uint32_t trial_hash_high;
    uint32_t rejected_source;
    uint32_t rejected_hash_low;
    uint32_t rejected_hash_high;
    uint32_t failure_stage;
    uint32_t failure_code;
} PjsLineageRecord;

typedef struct {
    uint32_t available;
    uint32_t active_slot;
    uint32_t error;
    PjsLineageRecord record;
} PjsLineageState;

int pjs_fat32_mount(PjsFat32 *fat, PjsSectorReadFn reader, void *context);
int pjs_fat32_find_short_directory(PjsFat32 *fat, uint32_t parent_cluster,
                                   const char directory_name[11],
                                   uint32_t *cluster_out);
int pjs_fat32_list_short_files(PjsFat32 *fat, uint32_t directory_cluster,
                               const char extension[3],
                               PjsStorageApp *entries, uint32_t capacity,
                               uint32_t *count_out);
int pjs_fat32_short_file_size_at(PjsFat32 *fat, uint32_t directory_cluster,
                                 const char file_name[11],
                                 uint32_t *size_out);
int pjs_fat32_read_short_file_at(PjsFat32 *fat, uint32_t directory_cluster,
                                 const char file_name[11],
                                 uint8_t *destination, uint32_t capacity,
                                 uint32_t *length_out);
int pjs_fat32_short_file_size(PjsFat32 *fat,
                              const char directory_name[11],
                              const char file_name[11],
                              uint32_t *size_out);
int pjs_fat32_read_short_file(PjsFat32 *fat,
                              const char directory_name[11],
                              const char file_name[11],
                              uint8_t *destination, uint32_t capacity,
                              uint32_t *length_out);
int pjs_fat32_short_file_sector(PjsFat32 *fat,
                                const char directory_name[11],
                                const char file_name[11],
                                uint32_t expected_size,
                                uint32_t *lba_out);
int pjs_fat32_short_file_sectors(PjsFat32 *fat,
                                 const char directory_name[11],
                                 const char file_name[11],
                                 uint32_t expected_size,
                                 uint32_t lba_out[PJS_STORAGE_MAX_FILE_SECTORS]);
/* Strict resolver for explicitly provisioned large files. The existing
 * short-file resolver remains limited to PJS_STORAGE_MAX_FILE_SECTORS. */
int pjs_fat32_short_file_sectors_bounded(
    PjsFat32 *fat, const char directory_name[11], const char file_name[11],
    uint32_t expected_size, uint32_t *lba_out, uint32_t lba_capacity);

void pjs_state_record_build(uint8_t record[PJS_STORAGE_SECTOR_BYTES],
                            uint32_t generation, uint32_t payload,
                            bool committed);
bool pjs_state_record_read(const uint8_t record[PJS_STORAGE_SECTOR_BYTES],
                           uint32_t *generation_out,
                           uint32_t *payload_out);
void pjs_lineage_record_build(uint8_t record[PJS_STORAGE_SECTOR_BYTES],
                              const PjsLineageRecord *lineage,
                              bool committed);
bool pjs_lineage_record_read(const uint8_t record[PJS_STORAGE_SECTOR_BYTES],
                             PjsLineageRecord *lineage_out);

int pjs_storage_load_guest(PjsStorageFile *file);
void pjs_storage_reset_diagnostics(void);
int pjs_storage_load_guest_named(PjsStorageFile *file,
                                 const char file_name[11]);
int pjs_storage_discover_apps(PjsStorageCatalog *catalog);
int pjs_storage_load_app(PjsStorageFile *file, const char file_name[11]);
void pjs_storage_release(PjsStorageFile *file);
uint32_t pjs_storage_last_error(void);
uint32_t pjs_storage_sector_read_count(void);
uint32_t pjs_storage_first_failed_lba(void);
uint32_t pjs_storage_sector_write_count(void);
uint32_t pjs_storage_sector_flush_count(void);
uint32_t pjs_storage_failed_operation(void);
uint32_t pjs_storage_failed_status(void);
uint32_t pjs_storage_failed_error(void);

/* Classify an ATA status byte without touching the task-file status register.
 * The handoff path uses ALT_STATUS so an error/status read cannot acknowledge
 * an in-flight request. */
uint8_t pjs_storage_ata_status_classify(uint8_t status);

/* Bounded, read-only running-firmware handoff preparation. These functions
 * never issue an ATA command, write a sector, alter FAT metadata, or control
 * PMU/charger/rail state. The caller owns the terminal reset after a READY
 * result and must clear the marker if it aborts that reset. */
int pjs_storage_ata_quiesce(PjsStorageDiskHandoff *handoff);
int pjs_storage_prepare_disk_handoff(PjsStorageDiskHandoff *handoff);
void pjs_storage_disk_handoff_clear(void);
bool pjs_storage_disk_handoff_armed(void);

/* Terminal storage sequencing. A successful OFF result is only possible
 * after FLUSH CACHE and STANDBY IMMEDIATE complete, and only this API may
 * disable the Photo's IDE0 disk rail. NOT_OWNED is a safe no-op for an
 * embedded-only boot with no disk transaction. */
int pjs_storage_disk_power_on(void);
int pjs_storage_disk_flush(PjsStorageDiskPower *power);
int pjs_storage_disk_standby(PjsStorageDiskPower *power);
int pjs_storage_disk_power_off(PjsStorageDiskPower *power);
int pjs_storage_disk_flush_standby_off(PjsStorageDiskPower *power);
uint8_t pjs_storage_disk_power_state(void);

int pjs_storage_state_load(PjsPersistenceState *state);
int pjs_storage_state_write(PjsPersistenceState *state, bool publish,
                            uint32_t *attempted_slot,
                            uint32_t *attempted_generation);
int pjs_storage_lineage_load(PjsLineageState *state);
int pjs_storage_lineage_write(PjsLineageState *state,
                              const PjsLineageRecord *record);

/* Raw sector callbacks for the data.fs bank files. The caller must own the
 * disk transaction and bracket a batch with flush. */
bool pjs_storage_sector_read(void *context, uint32_t lba,
                             uint8_t sector[PJS_STORAGE_SECTOR_BYTES]);
bool pjs_storage_sector_write(void *context, uint32_t lba,
                              const uint8_t sector[PJS_STORAGE_SECTOR_BYTES]);
bool pjs_storage_sector_flush(void *context);
int pjs_storage_resolve_short_file(void *context, const char file_name[11],
                                    uint32_t expected_size,
                                    uint32_t lba_out[PJS_STORAGE_MAX_FILE_SECTORS]);

/* Native, preallocated package-bank storage. Initialization only discovers
 * and validates existing FAT chains; it never creates files or changes FAT.
 * NOT_FOUND means the optional banks are unprovisioned and legacy boot may
 * continue. The caller owns lineage selection and ATA maintenance ownership. */
int pjs_storage_package_bank_init(void);
int pjs_storage_package_bank_inspect(uint32_t bank,
                                     PjsPackageStoreHeader *header);
int pjs_storage_load_package_bank(uint32_t bank,
                                  uint32_t expected_hash_low,
                                  uint32_t expected_hash_high,
                                  PjsStorageFile *file,
                                  PjsPackageStoreHeader *header);
int pjs_storage_stage_package_bank(uint32_t bank, uint32_t generation,
                                   uint32_t expected_payload_crc,
                                   uint32_t hash_low, uint32_t hash_high,
                                   const uint8_t *payload,
                                   uint32_t payload_length);

#endif
