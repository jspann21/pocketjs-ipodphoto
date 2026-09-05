#ifndef POCKETJS_IPOD_PHOTO_DATA_FS_H
#define POCKETJS_IPOD_PHOTO_DATA_FS_H

#include <stdbool.h>
#include <stdint.h>

/* Entry count is independent of the fixed bank size: the encoder still
 * rejects an image that exceeds the 136-sector bank.  Sixty-four slots leave
 * room for multiple app namespaces and directory metadata without changing
 * the compatible on-disk format. */
#define PJS_DATA_FS_MAX_FILES 64u
#define PJS_DATA_FS_MAX_FILE_BYTES 3072u
/* Internal names include an app namespace before the public 160-byte path. */
#define PJS_DATA_FS_MAX_PATH_BYTES 192u
#define PJS_DATA_FS_MAX_QUOTA_BYTES \
    (PJS_DATA_FS_MAX_FILES * PJS_DATA_FS_MAX_FILE_BYTES)
#define PJS_DATA_FS_SECTOR_BYTES 512u
#define PJS_DATA_FS_BANK_SECTORS 136u

typedef bool (*PjsDataFsReadSector)(
    void *, uint32_t, uint8_t[PJS_DATA_FS_SECTOR_BYTES]);
typedef bool (*PjsDataFsWriteSector)(
    void *, uint32_t, const uint8_t[PJS_DATA_FS_SECTOR_BYTES]);
typedef bool (*PjsDataFsFlush)(void *);
typedef int (*PjsDataFsResolveFile)(
    void *, const char[11], uint32_t,
    uint32_t[PJS_DATA_FS_BANK_SECTORS]);

typedef struct {
    char path[PJS_DATA_FS_MAX_PATH_BYTES];
    uint32_t size;
    bool directory;
} PjsDataFsEntry;

typedef struct {
    char path[PJS_DATA_FS_MAX_PATH_BYTES];
    uint8_t bytes[PJS_DATA_FS_MAX_FILE_BYTES];
    uint32_t size;
    bool used;
    bool directory;
} PjsDataFsFile;

typedef struct {
    PjsDataFsReadSector read_sector;
    PjsDataFsWriteSector write_sector;
    PjsDataFsFlush flush;
    void *context;
    uint32_t first_lba;
    uint32_t sector_count;
    uint32_t generation;
    uint32_t active_slot;
    uint32_t used_bytes;
    uint32_t quota_bytes;
    PjsDataFsFile files[PJS_DATA_FS_MAX_FILES];
} PjsDataFs;

enum {
    PJS_DATA_FS_OK = 0,
    PJS_DATA_FS_ERR_ARGUMENT = -1,
    PJS_DATA_FS_ERR_IO = -2,
    PJS_DATA_FS_ERR_FORMAT = -3,
    PJS_DATA_FS_ERR_NOT_FOUND = -4,
    PJS_DATA_FS_ERR_TOO_LARGE = -5,
    PJS_DATA_FS_ERR_QUOTA = -6,
    PJS_DATA_FS_ERR_FULL = -7,
    PJS_DATA_FS_ERR_PATH = -8,
    PJS_DATA_FS_ERR_VERIFY = -9,
};

int pjs_data_fs_open(PjsDataFs *, PjsDataFsReadSector,
                     PjsDataFsWriteSector, PjsDataFsFlush, void *,
                     uint32_t, uint32_t, uint32_t);
int pjs_data_fs_open_fat(PjsDataFs *, PjsDataFsReadSector,
                         PjsDataFsWriteSector, PjsDataFsFlush, void *,
                         PjsDataFsResolveFile, const char[11], const char[11],
                         uint32_t);
int pjs_data_fs_commit(PjsDataFs *);
int pjs_data_fs_read(PjsDataFs *, const char *, uint32_t, uint8_t *,
                     uint32_t, uint32_t *, uint32_t *);
int pjs_data_fs_write(PjsDataFs *, const char *, const uint8_t *, uint32_t,
                      bool);
int pjs_data_fs_remove(PjsDataFs *, const char *, bool);
int pjs_data_fs_mkdir(PjsDataFs *, const char *);
int pjs_data_fs_rename(PjsDataFs *, const char *, const char *);
int pjs_data_fs_stat(PjsDataFs *, const char *, PjsDataFsEntry *);
int pjs_data_fs_list(PjsDataFs *, const char *, uint32_t, PjsDataFsEntry *,
                     uint32_t, uint32_t *, bool *);
uint32_t pjs_data_fs_used(const PjsDataFs *);
bool pjs_data_fs_valid_path(const char *);

#endif
