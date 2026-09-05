#ifndef POCKETJS_IPOD_PHOTO_HEAP_H
#define POCKETJS_IPOD_PHOTO_HEAP_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

typedef struct {
    size_t total_bytes;
    size_t free_bytes;
    size_t largest_free;
    size_t allocated_bytes;
    size_t allocation_count;
    size_t free_block_count;
} PjsHeapStats;

bool pjs_heap_init(void *base, size_t length);
void *pjs_heap_alloc(size_t size, size_t alignment);
void *pjs_heap_realloc(void *pointer, size_t new_size, size_t alignment);
void pjs_heap_free(void *pointer);
size_t pjs_heap_usable_size(const void *pointer);
bool pjs_heap_validate(void);
void pjs_heap_stats(PjsHeapStats *stats);

#endif
