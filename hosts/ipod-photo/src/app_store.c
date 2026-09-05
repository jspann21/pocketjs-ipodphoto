#include "app_store.h"
#include "heap.h"
#include "usb_protocol.h"
#include <string.h>

#define INDEX_MAGIC 0x3141504du
#define INDEX_HASH 0x58444941u

typedef struct {
    char name[11];
    uint8_t state; /* 0 free, 1 installed, 2 hides a legacy app */
    uint32_t bank, length, low, high;
    char app_id[128];
} AppEntry;
typedef struct { uint32_t magic, version; AppEntry entries[PJS_APP_STORE_SLOTS]; } AppIndex;
_Static_assert(sizeof(AppEntry) == 156u, "app catalog wire layout changed");
_Static_assert(sizeof(AppIndex) == 1256u, "app catalog size changed");
static AppIndex current;
static uint32_t active_index, generation;
static bool mounted;

static bool valid_name(const char name[11])
{
    bool space = false, any = false;
    for (uint32_t i = 0; i < 8u; ++i) {
        char c = name[i];
        if (c == ' ') { space = true; continue; }
        if (space || !((c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '_' || c == '-')) return false;
        any = true;
    }
    return any && memcmp(name + 8, "PKT", 3) == 0;
}

static bool valid_index(const AppIndex *index)
{
    if (index->magic != INDEX_MAGIC || index->version != 1u) return false;
    for (uint32_t i = 0; i < PJS_APP_STORE_SLOTS; ++i) {
        const AppEntry *e = &index->entries[i];
        if (e->state == 0u) continue;
        if (e->state > 2u || !valid_name(e->name) || e->bank > 1u ||
            e->app_id[127] != 0) return false;
        if (e->state == 1u && (e->length == 0u ||
            e->length > PJS_PACKAGE_STORE_PAYLOAD_LIMIT || !e->app_id[0])) return false;
        for (uint32_t j = 0; j < i; ++j)
            if (index->entries[j].state && memcmp(e->name, index->entries[j].name, 11) == 0)
                return false;
    }
    return true;
}

static int mount_index(void)
{
    if (mounted) return PJS_STORAGE_OK;
    const PjsPackageStoreIo *io;
    int rc = pjs_storage_app_pair(PJS_APP_STORE_INDEX_SLOT, false, &io);
    if (rc != 0) return rc;
    bool found = false;
    for (uint32_t bank = 0; bank < 2u; ++bank) {
        PjsPackageStoreHeader h;
        AppIndex candidate;
        if (pjs_package_store_inspect_header(io, bank, &h) != 0 ||
            h.payload_length != sizeof(candidate) || h.hash_low != INDEX_HASH || h.hash_high != 1u)
            continue;
        if (pjs_package_store_load_exact(io, bank, INDEX_HASH, 1u,
            (uint8_t *)&candidate, sizeof(candidate), &h) != 0 || !valid_index(&candidate)) continue;
        if (!found || (int32_t)(h.generation - generation) > 0) {
            current = candidate;
            generation = h.generation;
            active_index = bank;
            found = true;
        }
    }
    mounted = found;
    return found ? PJS_STORAGE_OK : PJS_STORAGE_ERR_VERIFY;
}

static int publish(const AppIndex *next)
{
    const PjsPackageStoreIo *io;
    int rc = pjs_storage_app_pair(PJS_APP_STORE_INDEX_SLOT, true, &io);
    if (rc != 0) return rc;
    uint32_t bank = active_index ^ 1u;
    rc = pjs_package_store_stage(io, bank, generation + 1u,
        pjs_usb_protocol_crc32((const uint8_t *)next, sizeof(*next)), INDEX_HASH, 1u,
        (const uint8_t *)next, sizeof(*next));
    /* A failed acknowledgment may follow a durable header write. Reload
     * both copies before any subsequent mutation chooses an inactive bank. */
    mounted = false;
    if (rc != 0) return PJS_STORAGE_ERR_VERIFY;
    return mount_index();
}

int pjs_app_store_lookup(const char name[11], uint32_t *slot, uint32_t *state)
{
    int rc = mount_index();
    if (rc != 0) return rc;
    for (uint32_t i = 0; i < PJS_APP_STORE_SLOTS; ++i)
        if (current.entries[i].state && memcmp(current.entries[i].name, name, 11) == 0) {
            *slot = i; *state = current.entries[i].state;
            return PJS_STORAGE_OK;
        }
    return PJS_STORAGE_ERR_NOT_FOUND;
}

int pjs_app_store_merge(PjsStorageCatalog *catalog)
{
    int rc = mount_index();
    if (rc == PJS_STORAGE_ERR_NOT_FOUND) return 0; /* unprovisioned host */
    if (rc != 0) return rc;
    for (uint32_t i = 0; i < PJS_APP_STORE_SLOTS; ++i) {
        const AppEntry *e = &current.entries[i];
        if (!e->state) continue;
        uint32_t at = 0;
        while (at < catalog->count && memcmp(catalog->apps[at].file_name, e->name, 11) != 0) ++at;
        if (e->state == 2u) {
            if (at < catalog->count) catalog->apps[at] = catalog->apps[--catalog->count];
        } else {
            if (at == catalog->count) {
                if (at == PJS_STORAGE_MAX_APPS) return PJS_STORAGE_ERR_TOO_MANY;
                ++catalog->count;
            }
            memcpy(catalog->apps[at].file_name, e->name, 11);
            catalog->apps[at].size = e->length;
        }
    }
    return 0;
}

int pjs_app_store_hash(const char name[11], uint32_t *low, uint32_t *high)
{
    uint32_t slot, state;
    int rc = pjs_app_store_lookup(name, &slot, &state);
    if (rc != 0 || state != 1u) return PJS_STORAGE_ERR_NOT_FOUND;
    *low = current.entries[slot].low; *high = current.entries[slot].high;
    return 0;
}

int pjs_app_store_load(const char name[11], PjsStorageFile *file)
{
    uint32_t slot, state;
    int rc = pjs_app_store_lookup(name, &slot, &state);
    if (rc != 0) return rc;
    if (state != 1u) return PJS_STORAGE_ERR_NOT_FOUND;
    AppEntry e = current.entries[slot];
    const PjsPackageStoreIo *io;
    rc = pjs_storage_app_pair(slot, false, &io);
    if (rc != 0) return rc;
    uint8_t *bytes = pjs_heap_alloc(e.length, 16u);
    if (!bytes) return PJS_STORAGE_ERR_ALLOC;
    PjsPackageStoreHeader h;
    rc = pjs_package_store_load_exact(io, e.bank, e.low, e.high, bytes, e.length, &h);
    PjsGuestPackage guest;
    if (rc == 0 && (h.payload_length != e.length ||
        pjs_package_open_ipod_photo(bytes, e.length, &guest) != 0 ||
        guest.package_hash_low != e.low || guest.package_hash_high != e.high ||
        guest.app_id_length != strlen(e.app_id) || memcmp(guest.app_id, e.app_id, guest.app_id_length)))
        rc = PJS_STORAGE_ERR_VERIFY;
    if (rc != 0) { pjs_heap_free(bytes); return PJS_STORAGE_ERR_VERIFY; }
    file->bytes = bytes; file->length = e.length;
    return 0;
}

int pjs_app_store_install(const char name[11], const uint8_t *bytes,
                          uint32_t length, const PjsGuestPackage *guest)
{
    if (!valid_name(name) || !guest || !bytes || !length ||
        length > PJS_PACKAGE_STORE_PAYLOAD_LIMIT || !guest->app_id_length ||
        guest->app_id_length >= 128u) return PJS_STORAGE_ERR_ARGUMENT;
    int rc = mount_index();
    if (rc != 0) return rc;
    uint32_t slot = PJS_APP_STORE_SLOTS, free_slot = PJS_APP_STORE_SLOTS;
    for (uint32_t i = 0; i < PJS_APP_STORE_SLOTS; ++i) {
        const AppEntry *e = &current.entries[i];
        if (!e->state && free_slot == PJS_APP_STORE_SLOTS) free_slot = i;
        if (e->state && memcmp(e->name, name, 11) == 0) slot = i;
        if (e->state == 1u && strlen(e->app_id) == guest->app_id_length &&
            memcmp(e->app_id, guest->app_id, guest->app_id_length) == 0 &&
            memcmp(e->name, name, 11) != 0) return PJS_STORAGE_ERR_STATE;
    }
    if (slot == PJS_APP_STORE_SLOTS) slot = free_slot;
    if (slot == PJS_APP_STORE_SLOTS) return PJS_STORAGE_ERR_TOO_MANY;
    PjsStorageCatalog catalog;
    rc = pjs_storage_discover_apps(&catalog);
    if (rc != 0) return rc;
    if (catalog.count == PJS_STORAGE_MAX_APPS) {
        bool replacing = false;
        for (uint32_t i = 0; i < catalog.count; ++i)
            if (memcmp(catalog.apps[i].file_name, name, 11) == 0) replacing = true;
        if (!replacing) return PJS_STORAGE_ERR_TOO_MANY;
    }
    AppEntry old = current.entries[slot];
    if (old.state == 1u && (strlen(old.app_id) != guest->app_id_length ||
        memcmp(old.app_id, guest->app_id, guest->app_id_length))) return PJS_STORAGE_ERR_STATE;
    if (old.state == 1u && old.low == guest->package_hash_low &&
        old.high == guest->package_hash_high && old.length == length) {
        PjsStorageFile checked = {0};
        rc = pjs_app_store_load(name, &checked);
        pjs_storage_release(&checked);
        if (rc == 0) return 0;
    }
    const PjsPackageStoreIo *io;
    rc = pjs_storage_app_pair(slot, true, &io);
    if (rc != 0) return rc;
    uint32_t bank = old.state == 1u ? old.bank ^ 1u : 0u;
    rc = pjs_package_store_stage(io, bank, generation + 1u,
        pjs_usb_protocol_crc32(bytes, length), guest->package_hash_low,
        guest->package_hash_high, bytes, length);
    if (rc != 0) return PJS_STORAGE_ERR_VERIFY;
    AppIndex next = current;
    AppEntry *e = &next.entries[slot];
    memset(e, 0, sizeof(*e));
    memcpy(e->name, name, 11); e->state = 1u; e->bank = bank; e->length = length;
    e->low = guest->package_hash_low; e->high = guest->package_hash_high;
    memcpy(e->app_id, guest->app_id, guest->app_id_length);
    return publish(&next);
}

int pjs_app_store_remove(const char name[11])
{
    if (!valid_name(name)) return PJS_STORAGE_ERR_ARGUMENT;
    int rc = mount_index();
    if (rc != 0) return rc;
    uint32_t slot = PJS_APP_STORE_SLOTS;
    for (uint32_t i = 0; i < PJS_APP_STORE_SLOTS; ++i)
        if (current.entries[i].state && memcmp(current.entries[i].name, name, 11) == 0) slot = i;
    PjsStorageFile legacy = {0};
    rc = pjs_storage_load_legacy_app(&legacy, name);
    pjs_storage_release(&legacy);
    if (rc != 0 && rc != PJS_STORAGE_ERR_NOT_FOUND) return rc;
    bool hide = rc == 0;
    if (slot == PJS_APP_STORE_SLOTS && !hide) return 0;
    if (slot == PJS_APP_STORE_SLOTS)
        for (uint32_t i = 0; i < PJS_APP_STORE_SLOTS; ++i)
            if (!current.entries[i].state) { slot = i; break; }
    if (slot == PJS_APP_STORE_SLOTS) return PJS_STORAGE_ERR_TOO_MANY;
    if (current.entries[slot].state == 2u && hide) return 0;
    AppIndex next = current;
    memset(&next.entries[slot], 0, sizeof(AppEntry));
    if (hide) { memcpy(next.entries[slot].name, name, 11); next.entries[slot].state = 2u; }
    return publish(&next);
}
