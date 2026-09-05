#include "storage.h"

#include <stdint.h>

#define FAT32_PARTITION_CHS 0x0bu
#define FAT32_PARTITION_LBA 0x0cu
#define FAT32_PARTITION_HIDDEN 0x1bu
#define FAT32_PARTITION_HIDDEN_LBA 0x1cu
#define FAT32_ATTR_LONG_NAME 0x0fu
#define FAT32_ATTR_VOLUME_ID 0x08u
#define FAT32_ATTR_DIRECTORY 0x10u
#define FAT32_CLUSTER_BAD 0x0ffffff7u
#define FAT32_CLUSTER_EOC 0x0ffffff8u
#define FAT32_DIRECTORY_CLUSTER_LIMIT 64u
#define FAT32_CATALOG_CLUSTER_LIMIT 8u

static uint16_t le16(const uint8_t *bytes)
{
    return (uint16_t)bytes[0] | ((uint16_t)bytes[1] << 8);
}

static uint32_t le32(const uint8_t *bytes)
{
    return (uint32_t)bytes[0] |
           ((uint32_t)bytes[1] << 8) |
           ((uint32_t)bytes[2] << 16) |
           ((uint32_t)bytes[3] << 24);
}

static bool power_of_two(uint32_t value)
{
    return value != 0u && (value & (value - 1u)) == 0u;
}

static bool fat32_partition_type(uint8_t type)
{
    return type == FAT32_PARTITION_CHS || type == FAT32_PARTITION_LBA ||
           type == FAT32_PARTITION_HIDDEN || type == FAT32_PARTITION_HIDDEN_LBA;
}

static uint32_t divide_by_power_of_two(uint32_t value, uint32_t divisor)
{
    while (divisor > 1u) {
        value >>= 1;
        divisor >>= 1;
    }
    return value;
}

static bool looks_like_fat32_bpb(const uint8_t sector[PJS_STORAGE_SECTOR_BYTES])
{
    return sector[510] == 0x55u && sector[511] == 0xaau &&
           le16(sector + 11) == PJS_STORAGE_SECTOR_BYTES &&
           sector[16] != 0u && le16(sector + 17) == 0u &&
           le16(sector + 22) == 0u && le32(sector + 36) != 0u;
}

int pjs_fat32_mount(PjsFat32 *fat, PjsSectorReadFn reader, void *context)
{
    if (fat == 0 || reader == 0) return PJS_STORAGE_ERR_ARGUMENT;
    *fat = (PjsFat32){0};
    fat->read_sector = reader;
    fat->context = context;

    uint8_t sector[PJS_STORAGE_SECTOR_BYTES];
    if (!reader(context, 0u, sector)) return PJS_STORAGE_ERR_ATA;
    if (sector[510] != 0x55u || sector[511] != 0xaau) {
        return PJS_STORAGE_ERR_PARTITION;
    }

    uint32_t partition_lba = 0u;
    uint32_t partition_sectors = 0u;
    if (looks_like_fat32_bpb(sector)) {
        partition_sectors = le32(sector + 32);
    } else {
        for (uint32_t index = 0u; index < 4u; ++index) {
            const uint8_t *entry = sector + 446u + index * 16u;
            if (!fat32_partition_type(entry[4])) continue;
            uint32_t start = le32(entry + 8);
            uint32_t count = le32(entry + 12);
            if (start == 0u || count == 0u) continue;
            partition_lba = start;
            partition_sectors = count;
            break;
        }
        if (partition_lba == 0u || partition_sectors == 0u) {
            return PJS_STORAGE_ERR_PARTITION;
        }
        if (!reader(context, partition_lba, sector)) return PJS_STORAGE_ERR_ATA;
    }

    if (!looks_like_fat32_bpb(sector)) return PJS_STORAGE_ERR_BPB;
    uint32_t bytes_per_sector = le16(sector + 11);
    uint32_t sectors_per_cluster = sector[13];
    uint32_t reserved = le16(sector + 14);
    uint32_t fat_count = sector[16];
    uint32_t total = le16(sector + 19);
    if (total == 0u) total = le32(sector + 32);
    uint32_t fat_sectors = le32(sector + 36);
    uint32_t root_cluster = le32(sector + 44) & 0x0fffffffu;

    if (bytes_per_sector != PJS_STORAGE_SECTOR_BYTES ||
        !power_of_two(sectors_per_cluster) || sectors_per_cluster > 128u ||
        reserved == 0u || fat_count == 0u || fat_count > 2u ||
        fat_sectors == 0u || root_cluster < 2u || total == 0u) {
        return PJS_STORAGE_ERR_BPB;
    }
    if (partition_sectors != 0u && total > partition_sectors) {
        return PJS_STORAGE_ERR_BPB;
    }
    uint64_t partition_end = (uint64_t)partition_lba +
                             (partition_sectors != 0u ? partition_sectors : total);
    if (partition_end > (uint64_t)UINT32_MAX + 1u) {
        return PJS_STORAGE_ERR_BPB;
    }
    uint64_t overhead = (uint64_t)reserved + (uint64_t)fat_count * fat_sectors;
    if (overhead >= total) return PJS_STORAGE_ERR_BPB;
    uint32_t data_sectors = total - (uint32_t)overhead;
    uint32_t cluster_count = divide_by_power_of_two(data_sectors,
                                                    sectors_per_cluster);
    uint64_t fat_entries = ((uint64_t)fat_sectors * PJS_STORAGE_SECTOR_BYTES) / 4u;
    if (cluster_count < 65525u || fat_entries < (uint64_t)cluster_count + 2u) {
        return PJS_STORAGE_ERR_BPB;
    }
    if (root_cluster - 2u >= cluster_count) return PJS_STORAGE_ERR_BPB;

    fat->partition_lba = partition_lba;
    fat->partition_sectors = partition_sectors != 0u ? partition_sectors : total;
    uint64_t fat_lba = (uint64_t)partition_lba + reserved;
    uint64_t data_lba = (uint64_t)partition_lba + overhead;
    if (fat_lba > UINT32_MAX || data_lba > UINT32_MAX) {
        return PJS_STORAGE_ERR_BPB;
    }
    fat->fat_lba = (uint32_t)fat_lba;
    fat->data_lba = (uint32_t)data_lba;
    fat->fat_sectors = fat_sectors;
    fat->root_cluster = root_cluster;
    fat->cluster_count = cluster_count;
    fat->sectors_per_cluster = (uint8_t)sectors_per_cluster;
    fat->fat_count = (uint8_t)fat_count;
    return PJS_STORAGE_OK;
}

static bool cluster_valid(const PjsFat32 *fat, uint32_t cluster)
{
    return cluster >= 2u && cluster - 2u < fat->cluster_count;
}

static bool sector_range_valid(const PjsFat32 *fat, uint64_t first,
                               uint32_t count)
{
    uint64_t partition_first = fat->partition_lba;
    uint64_t partition_end = partition_first + fat->partition_sectors;
    uint64_t end = first + count;
    return count != 0u && first >= partition_first && first <= UINT32_MAX &&
           end > first && end <= partition_end &&
           end <= (uint64_t)UINT32_MAX + 1u;
}

static int next_cluster(const PjsFat32 *fat, uint32_t cluster,
                        uint32_t *next_out)
{
    if (!cluster_valid(fat, cluster) || next_out == 0) return PJS_STORAGE_ERR_CHAIN;
    uint64_t offset = (uint64_t)cluster * 4u;
    uint32_t sector_index = (uint32_t)(offset / PJS_STORAGE_SECTOR_BYTES);
    uint32_t byte_index = (uint32_t)(offset % PJS_STORAGE_SECTOR_BYTES);
    if (sector_index >= fat->fat_sectors || byte_index > PJS_STORAGE_SECTOR_BYTES - 4u) {
        return PJS_STORAGE_ERR_CHAIN;
    }
    uint8_t sector[PJS_STORAGE_SECTOR_BYTES];
    uint64_t lba = (uint64_t)fat->fat_lba + sector_index;
    if (!sector_range_valid(fat, lba, 1u)) return PJS_STORAGE_ERR_CHAIN;
    if (!fat->read_sector(fat->context, (uint32_t)lba, sector)) {
        return PJS_STORAGE_ERR_ATA;
    }
    uint32_t next = le32(sector + byte_index) & 0x0fffffffu;
    if (next == FAT32_CLUSTER_BAD || next < 2u) return PJS_STORAGE_ERR_CHAIN;
    *next_out = next;
    return PJS_STORAGE_OK;
}

static bool cluster_lba(const PjsFat32 *fat, uint32_t cluster,
                        uint32_t *lba_out)
{
    if (!cluster_valid(fat, cluster) || lba_out == 0) return false;
    uint64_t first = (uint64_t)fat->data_lba +
                     (uint64_t)(cluster - 2u) * fat->sectors_per_cluster;
    if (!sector_range_valid(fat, first, fat->sectors_per_cluster)) return false;
    *lba_out = (uint32_t)first;
    return true;
}

static bool name_matches(const uint8_t *entry, const char name[11])
{
    for (uint32_t index = 0u; index < 11u; ++index) {
        if (entry[index] != (uint8_t)name[index]) return false;
    }
    return true;
}

typedef struct {
    uint32_t cluster;
    uint32_t size;
    uint8_t attributes;
} FatEntry;

static int find_entry(PjsFat32 *fat, uint32_t directory_cluster,
                      const char name[11], FatEntry *result)
{
    if (result == 0 || !cluster_valid(fat, directory_cluster)) {
        return PJS_STORAGE_ERR_ARGUMENT;
    }
    uint32_t cluster = directory_cluster;
    uint32_t visited = 0u;
    uint8_t sector[PJS_STORAGE_SECTOR_BYTES];
    for (;;) {
        if (!cluster_valid(fat, cluster) ||
            visited++ >= FAT32_DIRECTORY_CLUSTER_LIMIT) {
            return PJS_STORAGE_ERR_CHAIN;
        }
        uint32_t first = 0u;
        if (!cluster_lba(fat, cluster, &first)) return PJS_STORAGE_ERR_CHAIN;
        for (uint32_t sec = 0u; sec < fat->sectors_per_cluster; ++sec) {
            if (!fat->read_sector(fat->context, first + sec, sector)) {
                return PJS_STORAGE_ERR_ATA;
            }
            for (uint32_t offset = 0u; offset < PJS_STORAGE_SECTOR_BYTES; offset += 32u) {
                const uint8_t *entry = sector + offset;
                if (entry[0] == 0x00u) return PJS_STORAGE_ERR_NOT_FOUND;
                if (entry[0] == 0xe5u) continue;
                uint8_t attributes = entry[11];
                if (attributes == FAT32_ATTR_LONG_NAME ||
                    (attributes & FAT32_ATTR_VOLUME_ID) != 0u) continue;
                if (!name_matches(entry, name)) continue;
                uint32_t high = le16(entry + 20);
                uint32_t low = le16(entry + 26);
                result->cluster = ((high << 16) | low) & 0x0fffffffu;
                result->size = le32(entry + 28);
                result->attributes = attributes;
                return PJS_STORAGE_OK;
            }
        }
        uint32_t next = 0u;
        int rc = next_cluster(fat, cluster, &next);
        if (rc != PJS_STORAGE_OK) return rc;
        if (next >= FAT32_CLUSTER_EOC) return PJS_STORAGE_ERR_NOT_FOUND;
        cluster = next;
    }
}

int pjs_fat32_find_short_directory(PjsFat32 *fat, uint32_t parent_cluster,
                                   const char directory_name[11],
                                   uint32_t *cluster_out)
{
    if (fat == 0 || directory_name == 0 || cluster_out == 0) {
        return PJS_STORAGE_ERR_ARGUMENT;
    }
    *cluster_out = 0u;
    FatEntry directory = {0};
    int rc = find_entry(fat, parent_cluster, directory_name, &directory);
    if (rc != PJS_STORAGE_OK) return rc;
    if ((directory.attributes & FAT32_ATTR_DIRECTORY) == 0u ||
        !cluster_valid(fat, directory.cluster)) return PJS_STORAGE_ERR_NOT_FOUND;
    *cluster_out = directory.cluster;
    return PJS_STORAGE_OK;
}

static bool extension_matches(const uint8_t *entry, const char extension[3])
{
    return entry[8] == (uint8_t)extension[0] &&
           entry[9] == (uint8_t)extension[1] &&
           entry[10] == (uint8_t)extension[2];
}

static bool supported_short_base(const uint8_t *entry)
{
    bool saw_character = false;
    bool saw_space = false;
    for (uint32_t index = 0u; index < 8u; ++index) {
        uint8_t character = entry[index];
        if (character == (uint8_t)' ') {
            saw_space = true;
            continue;
        }
        if (saw_space) return false;
        if (!((character >= (uint8_t)'A' && character <= (uint8_t)'Z') ||
              (character >= (uint8_t)'0' && character <= (uint8_t)'9') ||
              character == (uint8_t)'_' || character == (uint8_t)'-')) {
            return false;
        }
        saw_character = true;
    }
    return saw_character;
}

int pjs_fat32_list_short_files(PjsFat32 *fat, uint32_t directory_cluster,
                               const char extension[3],
                               PjsStorageApp *entries, uint32_t capacity,
                               uint32_t *count_out)
{
    if (fat == 0 || extension == 0 || entries == 0 || capacity == 0u ||
        count_out == 0 || !cluster_valid(fat, directory_cluster)) {
        return PJS_STORAGE_ERR_ARGUMENT;
    }
    *count_out = 0u;
    uint32_t cluster = directory_cluster;
    uint32_t visited = 0u;
    uint8_t sector[PJS_STORAGE_SECTOR_BYTES];
    for (;;) {
        if (!cluster_valid(fat, cluster) ||
            visited++ >= FAT32_CATALOG_CLUSTER_LIMIT) {
            return PJS_STORAGE_ERR_CHAIN;
        }
        uint32_t first = 0u;
        if (!cluster_lba(fat, cluster, &first)) return PJS_STORAGE_ERR_CHAIN;
        for (uint32_t sec = 0u; sec < fat->sectors_per_cluster; ++sec) {
            if (!fat->read_sector(fat->context, first + sec, sector)) {
                return PJS_STORAGE_ERR_ATA;
            }
            for (uint32_t offset = 0u; offset < PJS_STORAGE_SECTOR_BYTES;
                 offset += 32u) {
                const uint8_t *entry = sector + offset;
                if (entry[0] == 0x00u) return PJS_STORAGE_OK;
                if (entry[0] == 0xe5u) continue;
                uint8_t attributes = entry[11];
                if (attributes == FAT32_ATTR_LONG_NAME ||
                    (attributes & (FAT32_ATTR_VOLUME_ID | FAT32_ATTR_DIRECTORY)) != 0u ||
                    !extension_matches(entry, extension) ||
                    !supported_short_base(entry)) continue;
                uint32_t high = le16(entry + 20);
                uint32_t low = le16(entry + 26);
                uint32_t file_cluster = ((high << 16) | low) & 0x0fffffffu;
                uint32_t size = le32(entry + 28);
                if (size == 0u || !cluster_valid(fat, file_cluster)) continue;
                if (*count_out >= capacity) return PJS_STORAGE_ERR_TOO_MANY;
                PjsStorageApp *result = &entries[*count_out];
                for (uint32_t index = 0u; index < 11u; ++index) {
                    result->file_name[index] = (char)entry[index];
                }
                result->size = size;
                ++*count_out;
            }
        }
        uint32_t next = 0u;
        int rc = next_cluster(fat, cluster, &next);
        if (rc != PJS_STORAGE_OK) return rc;
        if (next >= FAT32_CLUSTER_EOC) return PJS_STORAGE_OK;
        cluster = next;
    }
}

int pjs_fat32_short_file_size_at(PjsFat32 *fat, uint32_t directory_cluster,
                                 const char file_name[11],
                                 uint32_t *size_out)
{
    if (fat == 0 || file_name == 0 || size_out == 0) {
        return PJS_STORAGE_ERR_ARGUMENT;
    }
    *size_out = 0u;
    FatEntry file = {0};
    int rc = find_entry(fat, directory_cluster, file_name, &file);
    if (rc != PJS_STORAGE_OK) return rc;
    if ((file.attributes & FAT32_ATTR_DIRECTORY) != 0u || file.size == 0u ||
        !cluster_valid(fat, file.cluster)) return PJS_STORAGE_ERR_NOT_FOUND;
    *size_out = file.size;
    return PJS_STORAGE_OK;
}

int pjs_fat32_short_file_size(PjsFat32 *fat,
                              const char directory_name[11],
                              const char file_name[11],
                              uint32_t *size_out)
{
    if (fat == 0 || directory_name == 0 || file_name == 0 || size_out == 0) {
        return PJS_STORAGE_ERR_ARGUMENT;
    }
    *size_out = 0u;
    uint32_t directory_cluster = 0u;
    int rc = pjs_fat32_find_short_directory(
        fat, fat->root_cluster, directory_name, &directory_cluster);
    if (rc != PJS_STORAGE_OK) return rc;
    return pjs_fat32_short_file_size_at(
        fat, directory_cluster, file_name, size_out);
}

int pjs_fat32_read_short_file_at(PjsFat32 *fat, uint32_t directory_cluster,
                                 const char file_name[11],
                                 uint8_t *destination, uint32_t capacity,
                                 uint32_t *length_out)
{
    if (fat == 0 || file_name == 0 || destination == 0 || length_out == 0 ||
        !cluster_valid(fat, directory_cluster)) return PJS_STORAGE_ERR_ARGUMENT;
    *length_out = 0u;
    FatEntry file = {0};
    int rc = find_entry(fat, directory_cluster, file_name, &file);
    if (rc != PJS_STORAGE_OK) return rc;
    if ((file.attributes & FAT32_ATTR_DIRECTORY) != 0u) return PJS_STORAGE_ERR_NOT_FOUND;
    if (file.size == 0u || file.size > capacity) return PJS_STORAGE_ERR_TOO_LARGE;
    if (!cluster_valid(fat, file.cluster)) return PJS_STORAGE_ERR_CHAIN;

    uint32_t remaining = file.size;
    uint32_t written = 0u;
    uint32_t cluster = file.cluster;
    uint32_t visited = 0u;
    uint8_t sector[PJS_STORAGE_SECTOR_BYTES];
    while (remaining != 0u) {
        if (!cluster_valid(fat, cluster) || visited++ >= fat->cluster_count) {
            return PJS_STORAGE_ERR_CHAIN;
        }
        uint32_t first = 0u;
        if (!cluster_lba(fat, cluster, &first)) return PJS_STORAGE_ERR_CHAIN;
        for (uint32_t sec = 0u; sec < fat->sectors_per_cluster && remaining != 0u; ++sec) {
            if (!fat->read_sector(fat->context, first + sec, sector)) {
                return PJS_STORAGE_ERR_ATA;
            }
            uint32_t copy = remaining < PJS_STORAGE_SECTOR_BYTES ?
                            remaining : PJS_STORAGE_SECTOR_BYTES;
            for (uint32_t index = 0u; index < copy; ++index) {
                destination[written + index] = sector[index];
            }
            written += copy;
            remaining -= copy;
        }
        if (remaining == 0u) break;
        uint32_t next = 0u;
        rc = next_cluster(fat, cluster, &next);
        if (rc != PJS_STORAGE_OK) return rc;
        if (next >= FAT32_CLUSTER_EOC) return PJS_STORAGE_ERR_SHORT_READ;
        cluster = next;
    }
    *length_out = written;
    return written == file.size ? PJS_STORAGE_OK : PJS_STORAGE_ERR_SHORT_READ;
}

int pjs_fat32_read_short_file(PjsFat32 *fat,
                              const char directory_name[11],
                              const char file_name[11],
                              uint8_t *destination, uint32_t capacity,
                              uint32_t *length_out)
{
    if (fat == 0 || directory_name == 0) return PJS_STORAGE_ERR_ARGUMENT;
    uint32_t directory_cluster = 0u;
    int rc = pjs_fat32_find_short_directory(
        fat, fat->root_cluster, directory_name, &directory_cluster);
    if (rc != PJS_STORAGE_OK) return rc;
    return pjs_fat32_read_short_file_at(
        fat, directory_cluster, file_name, destination, capacity, length_out);
}

int pjs_fat32_short_file_sector(PjsFat32 *fat,
                                const char directory_name[11],
                                const char file_name[11],
                                uint32_t expected_size,
                                uint32_t *lba_out)
{
    if (fat == 0 || directory_name == 0 || file_name == 0 ||
        expected_size == 0u || expected_size > PJS_STORAGE_SECTOR_BYTES ||
        lba_out == 0) return PJS_STORAGE_ERR_ARGUMENT;
    *lba_out = 0u;
    uint32_t directory_cluster = 0u;
    int rc = pjs_fat32_find_short_directory(
        fat, fat->root_cluster, directory_name, &directory_cluster);
    if (rc != PJS_STORAGE_OK) return rc;
    FatEntry file = {0};
    rc = find_entry(fat, directory_cluster, file_name, &file);
    if (rc != PJS_STORAGE_OK) return rc;
    if ((file.attributes & FAT32_ATTR_DIRECTORY) != 0u ||
        file.size != expected_size || !cluster_valid(fat, file.cluster) ||
        file.cluster == fat->root_cluster || file.cluster == directory_cluster) {
        return PJS_STORAGE_ERR_STATE;
    }
    uint32_t next = 0u;
    rc = next_cluster(fat, file.cluster, &next);
    if (rc != PJS_STORAGE_OK || next < FAT32_CLUSTER_EOC) {
        return PJS_STORAGE_ERR_STATE;
    }
    if (!cluster_lba(fat, file.cluster, lba_out)) return PJS_STORAGE_ERR_CHAIN;
    return PJS_STORAGE_OK;
}

int pjs_fat32_short_file_sectors(PjsFat32 *fat,
                                 const char directory_name[11],
                                 const char file_name[11],
                                 uint32_t expected_size,
                                 uint32_t lba_out[PJS_STORAGE_MAX_FILE_SECTORS])
{
    if (fat == 0 || directory_name == 0 || file_name == 0 ||
        expected_size == 0u || expected_size % PJS_STORAGE_SECTOR_BYTES != 0u ||
        expected_size / PJS_STORAGE_SECTOR_BYTES > PJS_STORAGE_MAX_FILE_SECTORS ||
        lba_out == 0) return PJS_STORAGE_ERR_ARGUMENT;
    uint32_t directory_cluster = 0u;
    int rc = pjs_fat32_find_short_directory(
        fat, fat->root_cluster, directory_name, &directory_cluster);
    if (rc != PJS_STORAGE_OK) return rc;
    FatEntry file = {0};
    rc = find_entry(fat, directory_cluster, file_name, &file);
    if (rc != PJS_STORAGE_OK) return rc;
    if ((file.attributes & FAT32_ATTR_DIRECTORY) != 0u ||
        file.size != expected_size || !cluster_valid(fat, file.cluster) ||
        file.cluster == fat->root_cluster || file.cluster == directory_cluster)
        return PJS_STORAGE_ERR_STATE;
    uint32_t needed = expected_size / PJS_STORAGE_SECTOR_BYTES;
    uint32_t written = 0u;
    uint32_t cluster = file.cluster;
    uint32_t visited = 0u;
    while (written < needed) {
        if (!cluster_valid(fat, cluster) || visited++ >= fat->cluster_count)
            return PJS_STORAGE_ERR_CHAIN;
        uint32_t first = 0u;
        if (!cluster_lba(fat, cluster, &first)) return PJS_STORAGE_ERR_CHAIN;
        for (uint32_t sec = 0u; sec < fat->sectors_per_cluster &&
             written < needed; ++sec) lba_out[written++] = first + sec;
        if (written == needed) break;
        uint32_t next = 0u;
        rc = next_cluster(fat, cluster, &next);
        if (rc != PJS_STORAGE_OK) return rc;
        if (next >= FAT32_CLUSTER_EOC) return PJS_STORAGE_ERR_SHORT_READ;
        cluster = next;
    }
    return PJS_STORAGE_OK;
}

int pjs_fat32_short_file_sectors_bounded(
    PjsFat32 *fat, const char directory_name[11], const char file_name[11],
    uint32_t expected_size, uint32_t *lba_out, uint32_t lba_capacity)
{
    if (fat == 0 || directory_name == 0 || file_name == 0 ||
        expected_size == 0u || lba_out == 0 || lba_capacity == 0u ||
        lba_capacity > PJS_STORAGE_MAX_BOUNDED_FILE_SECTORS ||
        expected_size > UINT32_MAX - (PJS_STORAGE_SECTOR_BYTES - 1u)) {
        return PJS_STORAGE_ERR_ARGUMENT;
    }
    uint32_t directory_cluster = 0u;
    int rc = pjs_fat32_find_short_directory(
        fat, fat->root_cluster, directory_name, &directory_cluster);
    if (rc != PJS_STORAGE_OK) return rc;
    FatEntry file = {0};
    rc = find_entry(fat, directory_cluster, file_name, &file);
    if (rc != PJS_STORAGE_OK) return rc;
    if ((file.attributes & FAT32_ATTR_DIRECTORY) != 0u ||
        file.size != expected_size || !cluster_valid(fat, file.cluster) ||
        file.cluster == fat->root_cluster || file.cluster == directory_cluster) {
        return PJS_STORAGE_ERR_STATE;
    }
    uint32_t needed = (expected_size + PJS_STORAGE_SECTOR_BYTES - 1u) /
                      PJS_STORAGE_SECTOR_BYTES;
    if (needed == 0u || needed > lba_capacity) return PJS_STORAGE_ERR_TOO_LARGE;
    uint32_t written = 0u;
    uint32_t cluster = file.cluster;
    while (written < needed) {
        if (!cluster_valid(fat, cluster) ||
            cluster == fat->root_cluster || cluster == directory_cluster) {
            return PJS_STORAGE_ERR_CHAIN;
        }
        uint32_t first = 0u;
        if (!cluster_lba(fat, cluster, &first)) return PJS_STORAGE_ERR_CHAIN;
        for (uint32_t sec = 0u; sec < fat->sectors_per_cluster &&
             written < needed; ++sec) {
            lba_out[written++] = first + sec;
        }
        uint32_t next = 0u;
        rc = next_cluster(fat, cluster, &next);
        if (rc != PJS_STORAGE_OK) return rc;
        if (written < needed && next >= FAT32_CLUSTER_EOC) {
            return PJS_STORAGE_ERR_SHORT_READ;
        }
        if (written == needed && next < FAT32_CLUSTER_EOC) {
            return PJS_STORAGE_ERR_CHAIN;
        }
        cluster = next;
    }
    return PJS_STORAGE_OK;
}
