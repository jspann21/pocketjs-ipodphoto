#ifndef POCKETJS_IPOD_PHOTO_CACHE_H
#define POCKETJS_IPOD_PHOTO_CACHE_H

#include <stdbool.h>
#include <stddef.h>

void cache_take_ownership_disabled(void);
bool cache_take_ownership_enabled(void);
bool cache_owned_enabled(void);
void cache_clean_range(const void *address, size_t length);

#endif
