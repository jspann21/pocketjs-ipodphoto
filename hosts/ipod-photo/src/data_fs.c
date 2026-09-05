#include "data_fs.h"

#include <stddef.h>

#define FS_MAGIC 0x32534650u
#define FS_VERSION 1u
#define FS_HEADER_BYTES 24u
#define FS_ENTRY_BYTES (PJS_DATA_FS_MAX_PATH_BYTES + 8u)
#define FS_IMAGE_BYTES \
    (PJS_DATA_FS_BANK_SECTORS * PJS_DATA_FS_SECTOR_BYTES)

static uint8_t bank_a[FS_IMAGE_BYTES];
static uint8_t bank_b[FS_IMAGE_BYTES];
static char rename_paths[PJS_DATA_FS_MAX_FILES][PJS_DATA_FS_MAX_PATH_BYTES];

typedef struct {
    PjsDataFsReadSector read;
    PjsDataFsWriteSector write;
    PjsDataFsFlush flush;
    void *context;
    uint32_t lba[2][PJS_DATA_FS_BANK_SECTORS];
} FatAdapter;

static FatAdapter fat_adapter;

static uint32_t get32(const uint8_t *p)
{
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) |
           ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

static void put32(uint8_t *p, uint32_t value)
{
    p[0] = (uint8_t)value;
    p[1] = (uint8_t)(value >> 8);
    p[2] = (uint8_t)(value >> 16);
    p[3] = (uint8_t)(value >> 24);
}

static uint32_t crc32_image(const uint8_t *p, uint32_t length)
{
    uint32_t crc = 0xffffffffu;
    for (uint32_t index = 0u; index < length; ++index) {
        uint8_t byte = index >= 20u && index < 24u ? 0u : p[index];
        crc ^= byte;
        for (uint32_t bit = 0u; bit < 8u; ++bit) {
            uint32_t mask = 0u - (crc & 1u);
            crc = (crc >> 1) ^ (0xedb88320u & mask);
        }
    }
    return ~crc;
}

static uint32_t string_length(const char *value)
{
    uint32_t length = 0u;
    if (value == 0) return 0u;
    while (length < PJS_DATA_FS_MAX_PATH_BYTES && value[length] != 0) {
        ++length;
    }
    return length;
}

static bool strings_equal(const char *left, const char *right)
{
    for (uint32_t i = 0u; i < PJS_DATA_FS_MAX_PATH_BYTES; ++i) {
        if (left[i] != right[i]) return false;
        if (left[i] == 0) return true;
    }
    return false;
}

static int strings_compare(const char *left, const char *right)
{
    for (uint32_t i = 0u; i < PJS_DATA_FS_MAX_PATH_BYTES; ++i) {
        uint8_t a = (uint8_t)left[i];
        uint8_t b = (uint8_t)right[i];
        if (a < b) return -1;
        if (a > b) return 1;
        if (a == 0u) return 0;
    }
    return 0;
}

static bool starts_with(const char *value, const char *prefix, uint32_t length)
{
    for (uint32_t i = 0u; i < length; ++i) {
        if (value[i] != prefix[i]) return false;
    }
    return true;
}

static bool copy_string(char *destination, const char *source)
{
    uint32_t length = string_length(source);
    if (length == 0u || length >= PJS_DATA_FS_MAX_PATH_BYTES) return false;
    for (uint32_t i = 0u; i <= length; ++i) destination[i] = source[i];
    for (uint32_t i = length + 1u; i < PJS_DATA_FS_MAX_PATH_BYTES; ++i) {
        destination[i] = 0;
    }
    return true;
}

bool pjs_data_fs_valid_path(const char *path)
{
    uint32_t length = string_length(path);
    if (path == 0 || length == 0u || length >= PJS_DATA_FS_MAX_PATH_BYTES) {
        return false;
    }
    uint32_t segment_bytes = 0u;
    uint32_t depth = 0u;
    for (uint32_t i = 0u; i <= length; ++i) {
        uint8_t c = (uint8_t)path[i];
        if (c == (uint8_t)'/' || c == 0u) {
            uint32_t start = i - segment_bytes;
            if (segment_bytes == 0u || segment_bytes > 64u ||
                (segment_bytes == 1u && path[start] == '.') ||
                (segment_bytes == 2u && path[start] == '.' &&
                 path[start + 1u] == '.')) return false;
            if (++depth > 8u) return false;
            segment_bytes = 0u;
            continue;
        }
        if (c < 0x20u || c == 0x7fu) return false;
        if (c < 0x80u) {
            ++segment_bytes;
            continue;
        }
        uint32_t continuation = 0u;
        uint32_t codepoint = 0u;
        if (c >= 0xc2u && c <= 0xdfu) {
            continuation = 1u;
            codepoint = c & 0x1fu;
        } else if (c >= 0xe0u && c <= 0xefu) {
            continuation = 2u;
            codepoint = c & 0x0fu;
        } else if (c >= 0xf0u && c <= 0xf4u) {
            continuation = 3u;
            codepoint = c & 0x07u;
        } else {
            return false;
        }
        if (i + continuation >= length) return false;
        for (uint32_t n = 0u; n < continuation; ++n) {
            uint8_t next = (uint8_t)path[++i];
            if ((next & 0xc0u) != 0x80u) return false;
            codepoint = (codepoint << 6) | (next & 0x3fu);
        }
        if ((continuation == 2u && codepoint < 0x800u) ||
            (continuation == 3u && codepoint < 0x10000u) ||
            (codepoint >= 0xd800u && codepoint <= 0xdfffu) ||
            codepoint > 0x10ffffu) return false;
        segment_bytes += continuation + 1u;
    }
    return true;
}

static int find_entry(const PjsDataFs *fs, const char *path)
{
    for (uint32_t i = 0u; i < PJS_DATA_FS_MAX_FILES; ++i) {
        if (fs->files[i].used && strings_equal(fs->files[i].path, path)) {
            return (int)i;
        }
    }
    return -1;
}

static int free_entry(const PjsDataFs *fs)
{
    for (uint32_t i = 0u; i < PJS_DATA_FS_MAX_FILES; ++i) {
        if (!fs->files[i].used) return (int)i;
    }
    return -1;
}

static void reset_files(PjsDataFs *fs)
{
    for (uint32_t i = 0u; i < PJS_DATA_FS_MAX_FILES; ++i) {
        fs->files[i] = (PjsDataFsFile){0};
    }
    fs->used_bytes = 0u;
}

static bool image_header_valid(const uint8_t *image, uint32_t *generation)
{
    uint32_t total = get32(image + 16u);
    if (get32(image) != FS_MAGIC || get32(image + 4u) != FS_VERSION ||
        get32(image + 12u) > PJS_DATA_FS_MAX_FILES ||
        total < FS_HEADER_BYTES || total > FS_IMAGE_BYTES ||
        get32(image + 20u) != crc32_image(image, total)) return false;
    *generation = get32(image + 8u);
    return true;
}

static bool image_sector_span(const uint8_t *image, uint32_t *sectors)
{
    uint32_t total = get32(image + 16u);
    if (get32(image) != FS_MAGIC || get32(image + 4u) != FS_VERSION ||
        get32(image + 12u) > PJS_DATA_FS_MAX_FILES ||
        total < FS_HEADER_BYTES || total > FS_IMAGE_BYTES) return false;
    *sectors = (total + PJS_DATA_FS_SECTOR_BYTES - 1u) /
        PJS_DATA_FS_SECTOR_BYTES;
    return *sectors != 0u && *sectors <= PJS_DATA_FS_BANK_SECTORS;
}

static bool parent_is_directory(const PjsDataFs *fs, const char *path)
{
    uint32_t length = string_length(path);
    while (length != 0u && path[length - 1u] != '/') --length;
    if (length == 0u) return true;
    char parent[PJS_DATA_FS_MAX_PATH_BYTES] = {0};
    for (uint32_t i = 0u; i + 1u < length; ++i) parent[i] = path[i];
    int index = find_entry(fs, parent);
    return index >= 0 && fs->files[index].directory;
}

static bool decode_image(PjsDataFs *fs, const uint8_t *image)
{
    uint32_t generation = 0u;
    if (!image_header_valid(image, &generation)) return false;
    uint32_t count = get32(image + 12u);
    uint32_t total = get32(image + 16u);
    uint32_t offset = FS_HEADER_BYTES;
    reset_files(fs);
    fs->generation = generation;
    for (uint32_t item = 0u; item < count; ++item) {
        if (offset + FS_ENTRY_BYTES > total) return false;
        const uint8_t *record = image + offset;
        uint32_t size = get32(record + PJS_DATA_FS_MAX_PATH_BYTES + 4u);
        bool directory = record[PJS_DATA_FS_MAX_PATH_BYTES] != 0u;
        if (size > PJS_DATA_FS_MAX_FILE_BYTES || (directory && size != 0u) ||
            offset + FS_ENTRY_BYTES + size > total) return false;
        char path[PJS_DATA_FS_MAX_PATH_BYTES];
        for (uint32_t i = 0u; i < PJS_DATA_FS_MAX_PATH_BYTES; ++i) {
            path[i] = (char)record[i];
        }
        if (!pjs_data_fs_valid_path(path) || find_entry(fs, path) >= 0) {
            return false;
        }
        PjsDataFsFile *file = &fs->files[item];
        if (!copy_string(file->path, path)) return false;
        file->used = true;
        file->directory = directory;
        file->size = size;
        for (uint32_t i = 0u; i < size; ++i) {
            file->bytes[i] = record[FS_ENTRY_BYTES + i];
        }
        if (!directory) {
            if (fs->used_bytes > PJS_DATA_FS_MAX_QUOTA_BYTES - size) {
                return false;
            }
            fs->used_bytes += size;
        }
        offset += FS_ENTRY_BYTES + size;
    }
    if (offset != total) return false;
    for (uint32_t i = 0u; i < PJS_DATA_FS_MAX_FILES; ++i) {
        if (fs->files[i].used && !parent_is_directory(fs, fs->files[i].path)) {
            return false;
        }
    }
    return fs->quota_bytes == 0u || fs->used_bytes <= fs->quota_bytes;
}

static bool read_bank(PjsDataFs *fs, uint32_t slot, uint8_t *output)
{
    for (uint32_t i = 0u; i < FS_IMAGE_BYTES; ++i) output[i] = 0u;
    uint32_t logical = fs->first_lba + slot * PJS_DATA_FS_BANK_SECTORS;
    if (!fs->read_sector(fs->context, logical, output)) return false;
    uint32_t sectors = 0u;
    if (!image_sector_span(output, &sectors)) return true;
    for (uint32_t sector = 1u; sector < sectors; ++sector) {
        logical = fs->first_lba +
            slot * PJS_DATA_FS_BANK_SECTORS + sector;
        if (!fs->read_sector(fs->context, logical,
                             output + sector * PJS_DATA_FS_SECTOR_BYTES)) {
            return false;
        }
    }
    return true;
}

int pjs_data_fs_open(PjsDataFs *fs, PjsDataFsReadSector read_sector,
                     PjsDataFsWriteSector write_sector, PjsDataFsFlush flush,
                     void *context, uint32_t first_lba, uint32_t sector_count,
                     uint32_t quota_bytes)
{
    if (fs == 0 || read_sector == 0 || write_sector == 0 ||
        sector_count < 2u * PJS_DATA_FS_BANK_SECTORS ||
        quota_bytes > PJS_DATA_FS_MAX_QUOTA_BYTES) {
        return PJS_DATA_FS_ERR_ARGUMENT;
    }
    *fs = (PjsDataFs){
        .read_sector = read_sector,
        .write_sector = write_sector,
        .flush = flush,
        .context = context,
        .first_lba = first_lba,
        .sector_count = sector_count,
        .quota_bytes = quota_bytes,
    };
    bool read_a = read_bank(fs, 0u, bank_a);
    bool read_b = read_bank(fs, 1u, bank_b);
    uint32_t generation_a = 0u;
    uint32_t generation_b = 0u;
    bool valid_a = read_a && image_header_valid(bank_a, &generation_a);
    bool valid_b = read_b && image_header_valid(bank_b, &generation_b);
    if (!valid_a && !valid_b) {
        reset_files(fs);
        return PJS_DATA_FS_OK;
    }
    bool select_b = valid_b &&
        (!valid_a || (int32_t)(generation_b - generation_a) > 0);
    if (decode_image(fs, select_b ? bank_b : bank_a)) {
        fs->active_slot = select_b ? 1u : 0u;
        return PJS_DATA_FS_OK;
    }
    /* Header/CRC validity is not enough: reject malformed directory graphs,
     * but retain service by falling back to the previous complete bank. */
    bool fallback_valid = select_b ? valid_a : valid_b;
    if (fallback_valid && decode_image(fs, select_b ? bank_a : bank_b)) {
        fs->active_slot = select_b ? 0u : 1u;
        return PJS_DATA_FS_OK;
    }
    reset_files(fs);
    fs->generation = 0u;
    fs->active_slot = 0u;
    return PJS_DATA_FS_ERR_FORMAT;
}

static bool fat_read(void *context, uint32_t logical,
                     uint8_t output[PJS_DATA_FS_SECTOR_BYTES])
{
    FatAdapter *adapter = (FatAdapter *)context;
    if (logical >= 2u * PJS_DATA_FS_BANK_SECTORS) return false;
    uint32_t slot = logical / PJS_DATA_FS_BANK_SECTORS;
    uint32_t sector = logical % PJS_DATA_FS_BANK_SECTORS;
    return adapter->read(adapter->context, adapter->lba[slot][sector], output);
}

static bool fat_write(void *context, uint32_t logical,
                      const uint8_t input[PJS_DATA_FS_SECTOR_BYTES])
{
    FatAdapter *adapter = (FatAdapter *)context;
    if (logical >= 2u * PJS_DATA_FS_BANK_SECTORS) return false;
    uint32_t slot = logical / PJS_DATA_FS_BANK_SECTORS;
    uint32_t sector = logical % PJS_DATA_FS_BANK_SECTORS;
    return adapter->write(adapter->context, adapter->lba[slot][sector], input);
}

static bool fat_flush(void *context)
{
    FatAdapter *adapter = (FatAdapter *)context;
    return adapter->flush == 0 || adapter->flush(adapter->context);
}

int pjs_data_fs_open_fat(PjsDataFs *fs, PjsDataFsReadSector read_sector,
                         PjsDataFsWriteSector write_sector,
                         PjsDataFsFlush flush, void *context,
                         PjsDataFsResolveFile resolve,
                         const char bank0[11], const char bank1[11],
                         uint32_t quota_bytes)
{
    if (fs == 0 || read_sector == 0 || write_sector == 0 || resolve == 0) {
        return PJS_DATA_FS_ERR_ARGUMENT;
    }
    fat_adapter = (FatAdapter){
        .read = read_sector,
        .write = write_sector,
        .flush = flush,
        .context = context,
    };
    if (resolve(context, bank0, PJS_DATA_FS_BANK_SECTORS,
                fat_adapter.lba[0]) != PJS_DATA_FS_OK ||
        resolve(context, bank1, PJS_DATA_FS_BANK_SECTORS,
                fat_adapter.lba[1]) != PJS_DATA_FS_OK) {
        return PJS_DATA_FS_ERR_IO;
    }
    /* Both transaction banks must be physically independent. Reject a
     * cross-linked or cyclic FAT chain before either bank can be written. */
    for (uint32_t left = 0u; left < 2u * PJS_DATA_FS_BANK_SECTORS; ++left) {
        uint32_t left_lba = fat_adapter.lba
            [left / PJS_DATA_FS_BANK_SECTORS]
            [left % PJS_DATA_FS_BANK_SECTORS];
        for (uint32_t right = left + 1u;
             right < 2u * PJS_DATA_FS_BANK_SECTORS; ++right) {
            uint32_t right_lba = fat_adapter.lba
                [right / PJS_DATA_FS_BANK_SECTORS]
                [right % PJS_DATA_FS_BANK_SECTORS];
            if (left_lba == right_lba) return PJS_DATA_FS_ERR_VERIFY;
        }
    }
    return pjs_data_fs_open(fs, fat_read, fat_write, fat_flush, &fat_adapter,
                            0u, 2u * PJS_DATA_FS_BANK_SECTORS, quota_bytes);
}

static uint32_t encoded_size(const PjsDataFs *fs)
{
    uint32_t size = FS_HEADER_BYTES;
    for (uint32_t i = 0u; i < PJS_DATA_FS_MAX_FILES; ++i) {
        if (fs->files[i].used) size += FS_ENTRY_BYTES + fs->files[i].size;
    }
    return size;
}

int pjs_data_fs_commit(PjsDataFs *fs)
{
    if (fs == 0 || fs->write_sector == 0) return PJS_DATA_FS_ERR_ARGUMENT;
    uint32_t total = encoded_size(fs);
    if (total > FS_IMAGE_BYTES) return PJS_DATA_FS_ERR_TOO_LARGE;
    for (uint32_t i = 0u; i < FS_IMAGE_BYTES; ++i) bank_a[i] = 0u;
    put32(bank_a, FS_MAGIC);
    put32(bank_a + 4u, FS_VERSION);
    uint32_t next_generation = fs->generation + 1u;
    put32(bank_a + 8u, next_generation);
    put32(bank_a + 16u, total);
    uint32_t offset = FS_HEADER_BYTES;
    uint32_t count = 0u;
    for (uint32_t i = 0u; i < PJS_DATA_FS_MAX_FILES; ++i) {
        const PjsDataFsFile *file = &fs->files[i];
        if (!file->used) continue;
        for (uint32_t n = 0u; n < PJS_DATA_FS_MAX_PATH_BYTES; ++n) {
            bank_a[offset + n] = (uint8_t)file->path[n];
        }
        bank_a[offset + PJS_DATA_FS_MAX_PATH_BYTES] =
            file->directory ? 1u : 0u;
        put32(bank_a + offset + PJS_DATA_FS_MAX_PATH_BYTES + 4u, file->size);
        for (uint32_t n = 0u; n < file->size; ++n) {
            bank_a[offset + FS_ENTRY_BYTES + n] = file->bytes[n];
        }
        offset += FS_ENTRY_BYTES + file->size;
        ++count;
    }
    put32(bank_a + 12u, count);
    put32(bank_a + 20u, crc32_image(bank_a, total));
    uint32_t slot = fs->active_slot ^ 1u;
    uint32_t sectors = (total + PJS_DATA_FS_SECTOR_BYTES - 1u) /
        PJS_DATA_FS_SECTOR_BYTES;
    for (uint32_t sector = 0u; sector < sectors; ++sector) {
        uint32_t logical = fs->first_lba +
            slot * PJS_DATA_FS_BANK_SECTORS + sector;
        if (!fs->write_sector(fs->context, logical,
                              bank_a + sector * PJS_DATA_FS_SECTOR_BYTES)) {
            return PJS_DATA_FS_ERR_IO;
        }
    }
    if (fs->flush != 0 && !fs->flush(fs->context)) return PJS_DATA_FS_ERR_IO;
    for (uint32_t i = 0u; i < FS_IMAGE_BYTES; ++i) bank_b[i] = 0u;
    for (uint32_t sector = 0u; sector < sectors; ++sector) {
        uint32_t logical = fs->first_lba +
            slot * PJS_DATA_FS_BANK_SECTORS + sector;
        if (!fs->read_sector(fs->context, logical,
                             bank_b + sector * PJS_DATA_FS_SECTOR_BYTES)) {
            return PJS_DATA_FS_ERR_IO;
        }
    }
    uint32_t verified_generation = 0u;
    if (!image_header_valid(bank_b, &verified_generation) ||
        verified_generation != next_generation) {
        return PJS_DATA_FS_ERR_VERIFY;
    }
    for (uint32_t i = 0u;
         i < sectors * PJS_DATA_FS_SECTOR_BYTES; ++i) {
        if (bank_a[i] != bank_b[i]) return PJS_DATA_FS_ERR_VERIFY;
    }
    fs->generation = next_generation;
    fs->active_slot = slot;
    return PJS_DATA_FS_OK;
}

static uint32_t count_missing_parents(const PjsDataFs *fs, const char *path,
                                      bool *collision)
{
    char parent[PJS_DATA_FS_MAX_PATH_BYTES] = {0};
    uint32_t missing = 0u;
    *collision = false;
    for (uint32_t i = 0u; path[i] != 0; ++i) {
        parent[i] = path[i];
        if (path[i] != '/') continue;
        parent[i] = 0;
        int found = find_entry(fs, parent);
        if (found >= 0) {
            if (!fs->files[found].directory) *collision = true;
        } else {
            ++missing;
        }
        parent[i] = '/';
    }
    return missing;
}

static uint32_t free_count(const PjsDataFs *fs)
{
    uint32_t count = 0u;
    for (uint32_t i = 0u; i < PJS_DATA_FS_MAX_FILES; ++i) {
        if (!fs->files[i].used) ++count;
    }
    return count;
}

static int create_missing_parents(PjsDataFs *fs, const char *path)
{
    char parent[PJS_DATA_FS_MAX_PATH_BYTES] = {0};
    for (uint32_t i = 0u; path[i] != 0; ++i) {
        parent[i] = path[i];
        if (path[i] != '/') continue;
        parent[i] = 0;
        if (find_entry(fs, parent) < 0) {
            int slot = free_entry(fs);
            if (slot < 0 || !copy_string(fs->files[slot].path, parent)) {
                return PJS_DATA_FS_ERR_FULL;
            }
            fs->files[slot].used = true;
            fs->files[slot].directory = true;
        }
        parent[i] = '/';
    }
    return PJS_DATA_FS_OK;
}

int pjs_data_fs_read(PjsDataFs *fs, const char *path, uint32_t offset,
                     uint8_t *output, uint32_t capacity,
                     uint32_t *length_out, uint32_t *size_out)
{
    if (fs == 0 || output == 0 || length_out == 0 || size_out == 0 ||
        capacity == 0u || !pjs_data_fs_valid_path(path)) {
        return PJS_DATA_FS_ERR_ARGUMENT;
    }
    int index = find_entry(fs, path);
    if (index < 0 || fs->files[index].directory) {
        return PJS_DATA_FS_ERR_NOT_FOUND;
    }
    const PjsDataFsFile *file = &fs->files[index];
    if (offset > file->size) return PJS_DATA_FS_ERR_ARGUMENT;
    uint32_t length = file->size - offset;
    if (length > capacity) length = capacity;
    for (uint32_t i = 0u; i < length; ++i) output[i] = file->bytes[offset + i];
    *length_out = length;
    *size_out = file->size;
    return PJS_DATA_FS_OK;
}

int pjs_data_fs_write(PjsDataFs *fs, const char *path, const uint8_t *bytes,
                      uint32_t length, bool append)
{
    if (fs == 0 || (bytes == 0 && length != 0u) ||
        !pjs_data_fs_valid_path(path)) return PJS_DATA_FS_ERR_ARGUMENT;
    int index = find_entry(fs, path);
    if (index >= 0 && fs->files[index].directory) return PJS_DATA_FS_ERR_ARGUMENT;
    uint32_t old_size = index >= 0 ? fs->files[index].size : 0u;
    uint32_t total = append ? old_size + length : length;
    if (total < length || total > PJS_DATA_FS_MAX_FILE_BYTES) {
        return PJS_DATA_FS_ERR_TOO_LARGE;
    }
    uint32_t used = fs->used_bytes - old_size + total;
    if (fs->quota_bytes != 0u && used > fs->quota_bytes) {
        return PJS_DATA_FS_ERR_QUOTA;
    }
    bool collision = false;
    uint32_t missing = count_missing_parents(fs, path, &collision);
    if (collision) return PJS_DATA_FS_ERR_PATH;
    if (missing + (index < 0 ? 1u : 0u) > free_count(fs)) {
        return PJS_DATA_FS_ERR_FULL;
    }
    int rc = create_missing_parents(fs, path);
    if (rc != PJS_DATA_FS_OK) return rc;
    if (index < 0) {
        index = free_entry(fs);
        if (index < 0 || !copy_string(fs->files[index].path, path)) {
            return PJS_DATA_FS_ERR_FULL;
        }
        fs->files[index].used = true;
    }
    PjsDataFsFile *file = &fs->files[index];
    uint32_t start = append ? file->size : 0u;
    for (uint32_t i = 0u; i < length; ++i) file->bytes[start + i] = bytes[i];
    file->size = total;
    fs->used_bytes = used;
    return PJS_DATA_FS_OK;
}

int pjs_data_fs_mkdir(PjsDataFs *fs, const char *path)
{
    if (fs == 0 || !pjs_data_fs_valid_path(path)) return PJS_DATA_FS_ERR_PATH;
    int existing = find_entry(fs, path);
    if (existing >= 0) {
        return fs->files[existing].directory ? PJS_DATA_FS_OK :
                                               PJS_DATA_FS_ERR_ARGUMENT;
    }
    bool collision = false;
    uint32_t missing = count_missing_parents(fs, path, &collision) + 1u;
    if (collision) return PJS_DATA_FS_ERR_PATH;
    if (missing > free_count(fs)) return PJS_DATA_FS_ERR_FULL;
    int rc = create_missing_parents(fs, path);
    if (rc != PJS_DATA_FS_OK) return rc;
    int slot = free_entry(fs);
    if (slot < 0 || !copy_string(fs->files[slot].path, path)) {
        return PJS_DATA_FS_ERR_FULL;
    }
    fs->files[slot].used = true;
    fs->files[slot].directory = true;
    return PJS_DATA_FS_OK;
}

int pjs_data_fs_remove(PjsDataFs *fs, const char *path, bool recursive)
{
    if (fs == 0 || !pjs_data_fs_valid_path(path)) {
        return PJS_DATA_FS_ERR_ARGUMENT;
    }
    int index = find_entry(fs, path);
    if (index < 0) return PJS_DATA_FS_ERR_NOT_FOUND;
    uint32_t length = string_length(path);
    bool has_children = false;
    for (uint32_t i = 0u; i < PJS_DATA_FS_MAX_FILES; ++i) {
        if (fs->files[i].used && starts_with(fs->files[i].path, path, length) &&
            fs->files[i].path[length] == '/') has_children = true;
    }
    if (has_children && !recursive) return PJS_DATA_FS_ERR_ARGUMENT;
    for (uint32_t i = 0u; i < PJS_DATA_FS_MAX_FILES; ++i) {
        bool selected = (int)i == index ||
            (recursive && fs->files[i].used &&
             starts_with(fs->files[i].path, path, length) &&
             fs->files[i].path[length] == '/');
        if (!selected) continue;
        if (!fs->files[i].directory) fs->used_bytes -= fs->files[i].size;
        fs->files[i] = (PjsDataFsFile){0};
    }
    return PJS_DATA_FS_OK;
}

int pjs_data_fs_rename(PjsDataFs *fs, const char *from, const char *to)
{
    if (fs == 0 || !pjs_data_fs_valid_path(from) ||
        !pjs_data_fs_valid_path(to)) return PJS_DATA_FS_ERR_ARGUMENT;
    int source = find_entry(fs, from);
    if (source < 0) return PJS_DATA_FS_ERR_NOT_FOUND;
    if (!parent_is_directory(fs, to)) return PJS_DATA_FS_ERR_PATH;
    uint32_t from_length = string_length(from);
    uint32_t to_length = string_length(to);
    if (fs->files[source].directory && starts_with(to, from, from_length) &&
        to[from_length] == '/') return PJS_DATA_FS_ERR_PATH;
    int destination = find_entry(fs, to);
    if (destination >= 0 && fs->files[destination].directory) {
        return PJS_DATA_FS_ERR_ARGUMENT;
    }
    bool moving[PJS_DATA_FS_MAX_FILES] = {false};
    for (uint32_t i = 0u; i < PJS_DATA_FS_MAX_FILES; ++i) {
        if (!fs->files[i].used) continue;
        bool selected = (int)i == source ||
            (starts_with(fs->files[i].path, from, from_length) &&
             fs->files[i].path[from_length] == '/');
        if (!selected) continue;
        moving[i] = true;
        uint32_t old_length = string_length(fs->files[i].path);
        uint32_t suffix = old_length - from_length;
        if (to_length + suffix >= PJS_DATA_FS_MAX_PATH_BYTES) {
            return PJS_DATA_FS_ERR_TOO_LARGE;
        }
        for (uint32_t n = 0u; n < to_length; ++n) rename_paths[i][n] = to[n];
        for (uint32_t n = 0u; n <= suffix; ++n) {
            rename_paths[i][to_length + n] =
                fs->files[i].path[from_length + n];
        }
        if (!pjs_data_fs_valid_path(rename_paths[i])) {
            return PJS_DATA_FS_ERR_PATH;
        }
    }
    for (uint32_t i = 0u; i < PJS_DATA_FS_MAX_FILES; ++i) {
        if (!moving[i]) continue;
        for (uint32_t j = 0u; j < PJS_DATA_FS_MAX_FILES; ++j) {
            if (!fs->files[j].used || moving[j] || (int)j == destination) continue;
            if (strings_equal(rename_paths[i], fs->files[j].path)) {
                return PJS_DATA_FS_ERR_ARGUMENT;
            }
        }
    }
    if (destination >= 0 && !moving[destination]) {
        fs->used_bytes -= fs->files[destination].size;
        fs->files[destination] = (PjsDataFsFile){0};
    }
    for (uint32_t i = 0u; i < PJS_DATA_FS_MAX_FILES; ++i) {
        if (moving[i]) (void)copy_string(fs->files[i].path, rename_paths[i]);
    }
    return PJS_DATA_FS_OK;
}

int pjs_data_fs_stat(PjsDataFs *fs, const char *path, PjsDataFsEntry *entry)
{
    if (fs == 0 || path == 0 || entry == 0) return PJS_DATA_FS_ERR_ARGUMENT;
    if (path[0] == 0) {
        *entry = (PjsDataFsEntry){.directory = true};
        return PJS_DATA_FS_OK;
    }
    int index = find_entry(fs, path);
    if (index < 0) return PJS_DATA_FS_ERR_NOT_FOUND;
    *entry = (PjsDataFsEntry){
        .size = fs->files[index].size,
        .directory = fs->files[index].directory,
    };
    (void)copy_string(entry->path, fs->files[index].path);
    return PJS_DATA_FS_OK;
}

static bool direct_child(const char *path, const char *directory,
                         uint32_t *name_offset)
{
    uint32_t directory_length = string_length(directory);
    uint32_t start = 0u;
    if (directory_length != 0u) {
        if (!starts_with(path, directory, directory_length) ||
            path[directory_length] != '/') return false;
        start = directory_length + 1u;
    }
    if (path[start] == 0) return false;
    for (uint32_t i = start; path[i] != 0; ++i) {
        if (path[i] == '/') return false;
    }
    *name_offset = start;
    return true;
}

int pjs_data_fs_list(PjsDataFs *fs, const char *directory, uint32_t offset,
                     PjsDataFsEntry *entries, uint32_t capacity,
                     uint32_t *count_out, bool *eof_out)
{
    if (fs == 0 || directory == 0 || entries == 0 || count_out == 0 ||
        eof_out == 0 || capacity == 0u) return PJS_DATA_FS_ERR_ARGUMENT;
    if (directory[0] != 0) {
        int index = find_entry(fs, directory);
        if (index < 0 || !fs->files[index].directory) {
            return PJS_DATA_FS_ERR_NOT_FOUND;
        }
    }
    bool emitted[PJS_DATA_FS_MAX_FILES] = {false};
    uint32_t seen = 0u;
    uint32_t written = 0u;
    for (;;) {
        int best = -1;
        uint32_t best_offset = 0u;
        for (uint32_t i = 0u; i < PJS_DATA_FS_MAX_FILES; ++i) {
            uint32_t current_offset = 0u;
            if (emitted[i] || !fs->files[i].used ||
                !direct_child(fs->files[i].path, directory, &current_offset)) {
                continue;
            }
            if (best < 0 || strings_compare(fs->files[i].path + current_offset,
                                            fs->files[best].path + best_offset) < 0) {
                best = (int)i;
                best_offset = current_offset;
            }
        }
        if (best < 0) break;
        emitted[best] = true;
        if (seen++ < offset) continue;
        if (written >= capacity) continue;
        entries[written] = (PjsDataFsEntry){
            .size = fs->files[best].size,
            .directory = fs->files[best].directory,
        };
        (void)copy_string(entries[written].path,
                          fs->files[best].path + best_offset);
        ++written;
    }
    *count_out = written;
    *eof_out = seen <= offset + written;
    return PJS_DATA_FS_OK;
}

uint32_t pjs_data_fs_used(const PjsDataFs *fs)
{
    return fs == 0 ? 0u : fs->used_bytes;
}
