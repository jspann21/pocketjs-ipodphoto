#include "fs_store.h"

#include <stdbool.h>
#include <string.h>

#include "data_fs.h"
#include "storage.h"

#define PJS_FS_PUBLIC_MAX_PATH 160u
#define PJS_FS_PUBLIC_MAX_IO 65536u
#define PJS_FS_APP_QUOTA PJS_DATA_FS_MAX_FILE_BYTES

static PjsDataFs store;
static bool store_open;
static uint32_t active_app_hash;
static int store_error;
static char app_prefix[10];
static uint8_t io_bytes[PJS_DATA_FS_MAX_FILE_BYTES];

static const char bank0[11] = {
    'F','S','B','A','N','K','0',' ','B','I','N',
};
static const char bank1[11] = {
    'F','S','B','A','N','K','1',' ','B','I','N',
};

static uint32_t hash_app_id(const uint8_t *bytes, uint32_t length)
{
    uint32_t hash = 2166136261u;
    for (uint32_t i = 0u; i < length; ++i) {
        hash ^= bytes[i];
        hash *= 16777619u;
    }
    return hash;
}

static void make_prefix(uint32_t hash)
{
    static const char hex[] = "0123456789abcdef";
    app_prefix[0] = 'a';
    for (uint32_t i = 0u; i < 8u; ++i) {
        app_prefix[i + 1u] = hex[(hash >> (28u - i * 4u)) & 0x0fu];
    }
    app_prefix[9] = 0;
}

static void set_error(int error)
{
    store_error = error;
}

static void clear_error(void)
{
    store_error = 0;
}

int pjs_fs_store_last_error_code(void)
{
    return store_error;
}

static int reopen_store(void)
{
    int rc = pjs_data_fs_open_fat(
        &store, pjs_storage_sector_read, pjs_storage_sector_write,
        pjs_storage_sector_flush, 0, pjs_storage_resolve_short_file,
        bank0, bank1, 0u);
    if (rc == PJS_DATA_FS_OK) make_prefix(active_app_hash);
    return rc;
}

static int commit_or_restore(void)
{
    int rc = pjs_data_fs_commit(&store);
    if (rc == PJS_DATA_FS_OK) {
        clear_error();
        return 0;
    }
    (void)reopen_store();
    set_error(rc);
    return rc;
}

static bool bounded_path(const char *path, bool allow_root)
{
    if (path == 0) return false;
    size_t length = 0u;
    while (length <= PJS_FS_PUBLIC_MAX_PATH && path[length] != 0) ++length;
    if (length > PJS_FS_PUBLIC_MAX_PATH) return false;
    if (length == 0u) return allow_root;
    return true;
}

static int map_path(const char *path, bool allow_root,
                    char output[PJS_DATA_FS_MAX_PATH_BYTES])
{
    if (!bounded_path(path, allow_root)) return PJS_DATA_FS_ERR_PATH;
    size_t prefix_length = strlen(app_prefix);
    size_t path_length = strlen(path);
    size_t total = prefix_length + (path_length == 0u ? 0u : 1u) + path_length;
    if (total >= PJS_DATA_FS_MAX_PATH_BYTES) return PJS_DATA_FS_ERR_PATH;
    memcpy(output, app_prefix, prefix_length);
    size_t offset = prefix_length;
    if (path_length != 0u) {
        output[offset++] = '/';
        memcpy(output + offset, path, path_length);
        offset += path_length;
    }
    output[offset] = 0;
    return pjs_data_fs_valid_path(output) ? PJS_DATA_FS_OK :
                                           PJS_DATA_FS_ERR_PATH;
}

static bool belongs_to_app(const char *path)
{
    for (uint32_t i = 0u; i < 9u; ++i) {
        if (path[i] != app_prefix[i]) return false;
    }
    return path[9] == 0 || path[9] == '/';
}

static uint32_t app_used_bytes(void)
{
    uint32_t used = 0u;
    for (uint32_t i = 0u; i < PJS_DATA_FS_MAX_FILES; ++i) {
        if (store.files[i].used && !store.files[i].directory &&
            belongs_to_app(store.files[i].path)) {
            used += store.files[i].size;
        }
    }
    return used;
}

static uint32_t output_text(char *output, uint32_t capacity, uint32_t offset,
                            const char *text)
{
    while (*text != 0 && offset + 1u < capacity) output[offset++] = *text++;
    return offset;
}

static uint32_t output_number(char *output, uint32_t capacity, uint32_t offset,
                              uint32_t value)
{
    char reversed[10];
    uint32_t length = 0u;
    do {
        reversed[length++] = (char)('0' + value % 10u);
        value /= 10u;
    } while (value != 0u && length < sizeof(reversed));
    while (length != 0u && offset + 1u < capacity) {
        output[offset++] = reversed[--length];
    }
    return offset;
}

static uint32_t output_escaped(char *output, uint32_t capacity,
                               uint32_t offset, const char *text)
{
    while (*text != 0 && offset + 2u < capacity) {
        if (*text == '"' || *text == '\\') output[offset++] = '\\';
        output[offset++] = *text++;
    }
    return offset;
}

static int finish_output(char *output, uint32_t capacity, uint32_t offset,
                         uint32_t *length_out)
{
    if (output == 0 || length_out == 0 || offset >= capacity) {
        return PJS_DATA_FS_ERR_TOO_LARGE;
    }
    output[offset] = 0;
    *length_out = offset;
    return PJS_DATA_FS_OK;
}

static const char base64_alphabet[] =
    "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";

static uint32_t output_base64(char *output, uint32_t capacity, uint32_t offset,
                              const uint8_t *bytes, uint32_t length)
{
    for (uint32_t i = 0u; i < length; i += 3u) {
        uint32_t value = (uint32_t)bytes[i] << 16;
        if (i + 1u < length) value |= (uint32_t)bytes[i + 1u] << 8;
        if (i + 2u < length) value |= bytes[i + 2u];
        if (offset + 4u >= capacity) return capacity;
        output[offset++] = base64_alphabet[(value >> 18) & 63u];
        output[offset++] = base64_alphabet[(value >> 12) & 63u];
        output[offset++] = i + 1u < length ?
            base64_alphabet[(value >> 6) & 63u] : '=';
        output[offset++] = i + 2u < length ? base64_alphabet[value & 63u] : '=';
    }
    return offset;
}

static int base64_value(char c)
{
    if (c >= 'A' && c <= 'Z') return c - 'A';
    if (c >= 'a' && c <= 'z') return c - 'a' + 26;
    if (c >= '0' && c <= '9') return c - '0' + 52;
    if (c == '+') return 62;
    if (c == '/') return 63;
    return -1;
}

static int append_utf8(uint8_t *output, uint32_t *length, uint32_t codepoint)
{
    uint32_t needed = codepoint < 0x80u ? 1u : codepoint < 0x800u ? 2u :
                      codepoint < 0x10000u ? 3u : 4u;
    if (*length + needed > PJS_DATA_FS_MAX_FILE_BYTES) {
        return PJS_DATA_FS_ERR_TOO_LARGE;
    }
    if (needed == 1u) {
        output[(*length)++] = (uint8_t)codepoint;
    } else if (needed == 2u) {
        output[(*length)++] = (uint8_t)(0xc0u | (codepoint >> 6));
        output[(*length)++] = (uint8_t)(0x80u | (codepoint & 0x3fu));
    } else if (needed == 3u) {
        output[(*length)++] = (uint8_t)(0xe0u | (codepoint >> 12));
        output[(*length)++] = (uint8_t)(0x80u | ((codepoint >> 6) & 0x3fu));
        output[(*length)++] = (uint8_t)(0x80u | (codepoint & 0x3fu));
    } else {
        output[(*length)++] = (uint8_t)(0xf0u | (codepoint >> 18));
        output[(*length)++] = (uint8_t)(0x80u | ((codepoint >> 12) & 0x3fu));
        output[(*length)++] = (uint8_t)(0x80u | ((codepoint >> 6) & 0x3fu));
        output[(*length)++] = (uint8_t)(0x80u | (codepoint & 0x3fu));
    }
    return PJS_DATA_FS_OK;
}

static int hex_digit(char c)
{
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}

static int decode_hex4(const char *input, uint32_t *value)
{
    uint32_t result = 0u;
    for (uint32_t i = 0u; i < 4u; ++i) {
        int digit = hex_digit(input[i]);
        if (digit < 0) return PJS_DATA_FS_ERR_FORMAT;
        result = (result << 4) | (uint32_t)digit;
    }
    *value = result;
    return PJS_DATA_FS_OK;
}

static int decode_json_string(const char *payload, size_t length,
                              uint32_t *output_length)
{
    if (length < 2u || payload[0] != '"' || payload[length - 1u] != '"') {
        return PJS_DATA_FS_ERR_FORMAT;
    }
    uint32_t written = 0u;
    for (size_t i = 1u; i + 1u < length; ++i) {
        uint8_t c = (uint8_t)payload[i];
        if (c != '\\') {
            if (c < 0x20u || written >= PJS_DATA_FS_MAX_FILE_BYTES) {
                return PJS_DATA_FS_ERR_FORMAT;
            }
            io_bytes[written++] = c;
            continue;
        }
        if (++i + 1u >= length) return PJS_DATA_FS_ERR_FORMAT;
        char escape = payload[i];
        if (escape == '"' || escape == '\\' || escape == '/') {
            io_bytes[written++] = (uint8_t)escape;
        } else if (escape == 'b' || escape == 'f' || escape == 'n' ||
                   escape == 'r' || escape == 't') {
            static const uint8_t values[] = {'\b','\f','\n','\r','\t'};
            const char names[] = {'b','f','n','r','t'};
            uint32_t selected = 0u;
            while (names[selected] != escape) ++selected;
            io_bytes[written++] = values[selected];
        } else if (escape == 'u') {
            if (i + 4u >= length) return PJS_DATA_FS_ERR_FORMAT;
            uint32_t codepoint = 0u;
            if (decode_hex4(payload + i + 1u, &codepoint) != PJS_DATA_FS_OK) {
                return PJS_DATA_FS_ERR_FORMAT;
            }
            i += 4u;
            if (codepoint >= 0xd800u && codepoint <= 0xdbffu) {
                if (i + 6u >= length || payload[i + 1u] != '\\' ||
                    payload[i + 2u] != 'u') return PJS_DATA_FS_ERR_FORMAT;
                uint32_t low = 0u;
                if (decode_hex4(payload + i + 3u, &low) != PJS_DATA_FS_OK ||
                    low < 0xdc00u || low > 0xdfffu) {
                    return PJS_DATA_FS_ERR_FORMAT;
                }
                codepoint = 0x10000u + ((codepoint - 0xd800u) << 10) +
                            (low - 0xdc00u);
                i += 6u;
            } else if (codepoint >= 0xdc00u && codepoint <= 0xdfffu) {
                return PJS_DATA_FS_ERR_FORMAT;
            }
            int rc = append_utf8(io_bytes, &written, codepoint);
            if (rc != PJS_DATA_FS_OK) return rc;
        } else {
            return PJS_DATA_FS_ERR_FORMAT;
        }
    }
    *output_length = written;
    return PJS_DATA_FS_OK;
}

static int decode_base64_payload(const char *payload, size_t length,
                                 uint32_t *output_length)
{
    static const char marker[] = "{\"$b\":\"";
    size_t marker_length = sizeof(marker) - 1u;
    if (length < marker_length + 2u ||
        memcmp(payload, marker, marker_length) != 0 ||
        payload[length - 2u] != '"' || payload[length - 1u] != '}') {
        return PJS_DATA_FS_ERR_FORMAT;
    }
    size_t encoded = length - marker_length - 2u;
    if ((encoded & 3u) != 0u) return PJS_DATA_FS_ERR_FORMAT;
    uint32_t written = 0u;
    for (size_t i = 0u; i < encoded; i += 4u) {
        char chars[4];
        for (uint32_t n = 0u; n < 4u; ++n) chars[n] = payload[marker_length + i + n];
        int a = base64_value(chars[0]);
        int b = base64_value(chars[1]);
        int c = chars[2] == '=' ? 0 : base64_value(chars[2]);
        int d = chars[3] == '=' ? 0 : base64_value(chars[3]);
        if (a < 0 || b < 0 || c < 0 || d < 0 ||
            (chars[2] == '=' && chars[3] != '=') ||
            ((chars[2] == '=' || chars[3] == '=') && i + 4u != encoded)) {
            return PJS_DATA_FS_ERR_FORMAT;
        }
        uint32_t value = ((uint32_t)a << 18) | ((uint32_t)b << 12) |
                         ((uint32_t)c << 6) | (uint32_t)d;
        uint32_t count = chars[2] == '=' ? 1u : chars[3] == '=' ? 2u : 3u;
        if (written + count > PJS_DATA_FS_MAX_FILE_BYTES) {
            return PJS_DATA_FS_ERR_TOO_LARGE;
        }
        io_bytes[written++] = (uint8_t)(value >> 16);
        if (count > 1u) io_bytes[written++] = (uint8_t)(value >> 8);
        if (count > 2u) io_bytes[written++] = (uint8_t)value;
    }
    *output_length = written;
    return PJS_DATA_FS_OK;
}

static int decode_payload(const char *payload, size_t length,
                          uint32_t *output_length)
{
    if (payload == 0 || output_length == 0) return PJS_DATA_FS_ERR_ARGUMENT;
    return length != 0u && payload[0] == '"' ?
        decode_json_string(payload, length, output_length) :
        decode_base64_payload(payload, length, output_length);
}

int pjs_fs_store_open(const uint8_t *app_id, uint32_t length)
{
    store_open = false;
    if (app_id == 0 || length == 0u) {
        set_error(PJS_DATA_FS_ERR_ARGUMENT);
        return PJS_DATA_FS_ERR_ARGUMENT;
    }
    active_app_hash = hash_app_id(app_id, length);
    make_prefix(active_app_hash);
    int rc = reopen_store();
    if (rc != PJS_DATA_FS_OK) {
        set_error(rc);
        return rc;
    }
    store_open = true;
    int existing = -1;
    for (uint32_t i = 0u; i < PJS_DATA_FS_MAX_FILES; ++i) {
        if (store.files[i].used && strcmp(store.files[i].path, app_prefix) == 0) {
            existing = (int)i;
            break;
        }
    }
    if (existing >= 0 && !store.files[existing].directory) {
        set_error(PJS_DATA_FS_ERR_PATH);
        store_open = false;
        return PJS_DATA_FS_ERR_PATH;
    }
    if (existing < 0) {
        rc = pjs_data_fs_mkdir(&store, app_prefix);
        if (rc != PJS_DATA_FS_OK) {
            set_error(rc);
            store_open = false;
            return rc;
        }
    }
    clear_error();
    return PJS_DATA_FS_OK;
}

void pjs_fs_store_close(void)
{
    store_open = false;
    store = (PjsDataFs){0};
}

int pjs_fs_read_json(const char *path, uint32_t offset, uint32_t max_bytes,
                     char *output, uint32_t capacity, uint32_t *length_out)
{
    char mapped[PJS_DATA_FS_MAX_PATH_BYTES];
    if (!store_open || max_bytes == 0u || max_bytes > PJS_FS_PUBLIC_MAX_IO ||
        map_path(path, false, mapped) != PJS_DATA_FS_OK) {
        set_error(PJS_DATA_FS_ERR_ARGUMENT);
        return PJS_DATA_FS_ERR_ARGUMENT;
    }
    uint32_t request = max_bytes;
    if (request > sizeof(io_bytes)) request = sizeof(io_bytes);
    uint32_t read = 0u;
    uint32_t total = 0u;
    int rc = pjs_data_fs_read(&store, mapped, offset, io_bytes, request,
                              &read, &total);
    if (rc != PJS_DATA_FS_OK) {
        set_error(rc);
        return rc;
    }
    uint32_t length = output_text(output, capacity, 0u, "{\"data\":{\"$b\":\"");
    length = output_base64(output, capacity, length, io_bytes, read);
    length = output_text(output, capacity, length, "\"},\"size\":");
    length = output_number(output, capacity, length, total);
    length = output_text(output, capacity, length, ",\"eof\":");
    length = output_text(output, capacity, length,
                         offset + read >= total ? "true}" : "false}");
    rc = finish_output(output, capacity, length, length_out);
    if (rc == PJS_DATA_FS_OK) clear_error(); else set_error(rc);
    return rc;
}

int pjs_fs_write(const char *path, const char *payload, size_t payload_length,
                 int32_t mode)
{
    char mapped[PJS_DATA_FS_MAX_PATH_BYTES];
    if (!store_open || (mode != 0 && mode != 1) ||
        map_path(path, false, mapped) != PJS_DATA_FS_OK) {
        set_error(PJS_DATA_FS_ERR_ARGUMENT);
        return PJS_DATA_FS_ERR_ARGUMENT;
    }
    uint32_t decoded = 0u;
    int rc = decode_payload(payload, payload_length, &decoded);
    if (rc != PJS_DATA_FS_OK) {
        set_error(rc);
        return rc;
    }
    PjsDataFsEntry existing = {0};
    uint32_t old_size = pjs_data_fs_stat(&store, mapped, &existing) ==
        PJS_DATA_FS_OK && !existing.directory ? existing.size : 0u;
    uint32_t next_size = mode == 1 ? old_size + decoded : decoded;
    uint32_t used = app_used_bytes();
    if (next_size < decoded || used - old_size + next_size > PJS_FS_APP_QUOTA) {
        set_error(PJS_DATA_FS_ERR_QUOTA);
        return PJS_DATA_FS_ERR_QUOTA;
    }
    rc = pjs_data_fs_write(&store, mapped, io_bytes, decoded, mode == 1);
    if (rc != PJS_DATA_FS_OK) {
        (void)reopen_store();
        set_error(rc);
        return rc;
    }
    return commit_or_restore();
}

int pjs_fs_remove(const char *path, int32_t recursive)
{
    char mapped[PJS_DATA_FS_MAX_PATH_BYTES];
    if (!store_open || path == 0 || path[0] == 0 ||
        (recursive != 0 && recursive != 1) ||
        map_path(path, false, mapped) != PJS_DATA_FS_OK) {
        set_error(PJS_DATA_FS_ERR_ARGUMENT);
        return PJS_DATA_FS_ERR_ARGUMENT;
    }
    int rc = pjs_data_fs_remove(&store, mapped, recursive == 1);
    if (rc != PJS_DATA_FS_OK) {
        (void)reopen_store();
        set_error(rc);
        return rc;
    }
    return commit_or_restore();
}

int pjs_fs_mkdir(const char *path)
{
    char mapped[PJS_DATA_FS_MAX_PATH_BYTES];
    if (!store_open || map_path(path, false, mapped) != PJS_DATA_FS_OK) {
        set_error(PJS_DATA_FS_ERR_ARGUMENT);
        return PJS_DATA_FS_ERR_ARGUMENT;
    }
    int rc = pjs_data_fs_mkdir(&store, mapped);
    if (rc != PJS_DATA_FS_OK) {
        (void)reopen_store();
        set_error(rc);
        return rc;
    }
    return commit_or_restore();
}

int pjs_fs_rename(const char *from, const char *to)
{
    char mapped_from[PJS_DATA_FS_MAX_PATH_BYTES];
    char mapped_to[PJS_DATA_FS_MAX_PATH_BYTES];
    if (!store_open || map_path(from, false, mapped_from) != PJS_DATA_FS_OK ||
        map_path(to, false, mapped_to) != PJS_DATA_FS_OK) {
        set_error(PJS_DATA_FS_ERR_ARGUMENT);
        return PJS_DATA_FS_ERR_ARGUMENT;
    }
    int rc = pjs_data_fs_rename(&store, mapped_from, mapped_to);
    if (rc != PJS_DATA_FS_OK) {
        (void)reopen_store();
        set_error(rc);
        return rc;
    }
    return commit_or_restore();
}

int pjs_fs_stat_json(const char *path, char *output, uint32_t capacity,
                     uint32_t *length_out)
{
    char mapped[PJS_DATA_FS_MAX_PATH_BYTES];
    if (!store_open || map_path(path, true, mapped) != PJS_DATA_FS_OK) {
        set_error(PJS_DATA_FS_ERR_ARGUMENT);
        return PJS_DATA_FS_ERR_ARGUMENT;
    }
    PjsDataFsEntry entry = {0};
    int rc = pjs_data_fs_stat(&store, mapped, &entry);
    if (rc != PJS_DATA_FS_OK) {
        set_error(rc);
        return rc;
    }
    uint32_t length = output_text(output, capacity, 0u, "{\"kind\":\"");
    length = output_text(output, capacity, length,
                         entry.directory ? "dir" : "file");
    length = output_text(output, capacity, length, "\",\"size\":");
    length = output_number(output, capacity, length, entry.size);
    length = output_text(output, capacity, length, "}");
    rc = finish_output(output, capacity, length, length_out);
    if (rc == PJS_DATA_FS_OK) clear_error(); else set_error(rc);
    return rc;
}

int pjs_fs_list_json(const char *path, uint32_t offset, char *output,
                     uint32_t capacity, uint32_t *length_out)
{
    char mapped[PJS_DATA_FS_MAX_PATH_BYTES];
    if (!store_open || map_path(path, true, mapped) != PJS_DATA_FS_OK) {
        set_error(PJS_DATA_FS_ERR_ARGUMENT);
        return PJS_DATA_FS_ERR_ARGUMENT;
    }
    PjsDataFsEntry entries[PJS_DATA_FS_MAX_FILES];
    uint32_t count = 0u;
    bool eof = false;
    int rc = pjs_data_fs_list(&store, mapped, offset, entries,
                              PJS_DATA_FS_MAX_FILES, &count, &eof);
    if (rc != PJS_DATA_FS_OK) {
        set_error(rc);
        return rc;
    }
    uint32_t length = output_text(output, capacity, 0u, "{\"entries\":[");
    for (uint32_t i = 0u; i < count; ++i) {
        if (i != 0u) length = output_text(output, capacity, length, ",");
        length = output_text(output, capacity, length, "{\"name\":\"");
        length = output_escaped(output, capacity, length, entries[i].path);
        length = output_text(output, capacity, length, "\",\"kind\":\"");
        length = output_text(output, capacity, length,
                             entries[i].directory ? "dir" : "file");
        length = output_text(output, capacity, length, "\",\"size\":");
        length = output_number(output, capacity, length, entries[i].size);
        length = output_text(output, capacity, length, "}");
    }
    length = output_text(output, capacity, length,
                         eof ? "],\"eof\":true}" : "],\"eof\":false}");
    rc = finish_output(output, capacity, length, length_out);
    if (rc == PJS_DATA_FS_OK) clear_error(); else set_error(rc);
    return rc;
}

int pjs_fs_usage_json(char *output, uint32_t capacity, uint32_t *length_out)
{
    if (!store_open) {
        set_error(PJS_DATA_FS_ERR_ARGUMENT);
        return PJS_DATA_FS_ERR_ARGUMENT;
    }
    uint32_t length = output_text(output, capacity, 0u, "{\"usedBytes\":");
    length = output_number(output, capacity, length, app_used_bytes());
    length = output_text(output, capacity, length, ",\"quotaBytes\":");
    length = output_number(output, capacity, length, PJS_FS_APP_QUOTA);
    length = output_text(output, capacity, length, "}");
    int rc = finish_output(output, capacity, length, length_out);
    if (rc == PJS_DATA_FS_OK) clear_error(); else set_error(rc);
    return rc;
}

static const char *error_text(int error)
{
    switch (error) {
    case 0: return "";
    case PJS_DATA_FS_ERR_IO: return "I/O error";
    case PJS_DATA_FS_ERR_FORMAT: return "invalid store";
    case PJS_DATA_FS_ERR_NOT_FOUND: return "not found";
    case PJS_DATA_FS_ERR_TOO_LARGE: return "too large";
    case PJS_DATA_FS_ERR_QUOTA: return "quota exceeded";
    case PJS_DATA_FS_ERR_FULL: return "store full";
    case PJS_DATA_FS_ERR_PATH: return "invalid path";
    case PJS_DATA_FS_ERR_VERIFY: return "verification failed";
    default: return "invalid argument";
    }
}

int pjs_fs_last_error(char *output, uint32_t capacity, uint32_t *length_out)
{
    const char *text = error_text(store_error);
    uint32_t length = output_text(output, capacity, 0u, text);
    return finish_output(output, capacity, length, length_out);
}
