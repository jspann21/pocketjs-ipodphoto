#ifndef POCKETJS_IPOD_PHOTO_APP_STORE_H
#define POCKETJS_IPOD_PHOTO_APP_STORE_H

#include "storage.h"
#include "core_bridge.h"

#define PJS_APP_STORE_SLOTS 8u
#define PJS_APP_STORE_INDEX_SLOT PJS_APP_STORE_SLOTS
#define PJS_APP_STORE_INDEX_BYTES 2048u

int pjs_app_store_merge(PjsStorageCatalog *catalog);
int pjs_app_store_lookup(const char name[11], uint32_t *slot, uint32_t *state);
int pjs_app_store_hash(const char name[11], uint32_t *low, uint32_t *high);
int pjs_app_store_load(const char name[11], PjsStorageFile *file);
int pjs_app_store_install(const char name[11], const uint8_t *bytes,
                          uint32_t length, const PjsGuestPackage *guest);
int pjs_app_store_remove(const char name[11]);

#endif
