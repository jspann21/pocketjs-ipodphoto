#include "heap.h"

#include <stdint.h>

#define PJS_HEAP_BLOCK_MAGIC 0x504a4842u /* PJHB */
#define PJS_HEAP_PREFIX_MAGIC 0x504a4850u /* PJHP */
#define PJS_HEAP_MIN_ALIGN 16u
#define PJS_HEAP_MIN_PAYLOAD 32u

typedef struct PjsHeapBlock PjsHeapBlock;

struct PjsHeapBlock {
    uint32_t magic;
    uint32_t free;
    size_t span;
    PjsHeapBlock *previous;
    PjsHeapBlock *next;
};

typedef struct {
    uint32_t magic;
    uint32_t reserved;
    PjsHeapBlock *block;
    size_t requested;
} PjsHeapPrefix;

static uintptr_t heap_begin;
static uintptr_t heap_end;
static PjsHeapBlock *heap_first;
/* Allocation search resumes at the block after the last successful search.
 * QuickJS creates a large number of short-lived objects while parsing and
 * reconciling a generated app; restarting at heap_first makes that workload
 * repeatedly validate every permanent live block. */
static PjsHeapBlock *heap_cursor;

static uintptr_t align_up(uintptr_t value, size_t alignment)
{
    return (value + alignment - 1u) & ~(uintptr_t)(alignment - 1u);
}

static uintptr_t align_down(uintptr_t value, size_t alignment)
{
    return value & ~(uintptr_t)(alignment - 1u);
}

static bool alignment_valid(size_t alignment)
{
    return alignment != 0u && (alignment & (alignment - 1u)) == 0u &&
           alignment <= 65536u;
}

static size_t header_bytes(void)
{
    return (size_t)align_up(sizeof(PjsHeapBlock), PJS_HEAP_MIN_ALIGN);
}

static size_t prefix_bytes(void)
{
    return (size_t)align_up(sizeof(PjsHeapPrefix), PJS_HEAP_MIN_ALIGN);
}

static uintptr_t block_end(const PjsHeapBlock *block)
{
    return (uintptr_t)block + block->span;
}

static bool block_sane(const PjsHeapBlock *block)
{
    if (block == 0) return false;
    uintptr_t address = (uintptr_t)block;
    if (address < heap_begin || address >= heap_end) return false;
    if ((address & (PJS_HEAP_MIN_ALIGN - 1u)) != 0u) return false;
    if (heap_end - address < sizeof(PjsHeapBlock)) return false;
    if (block->magic != PJS_HEAP_BLOCK_MAGIC) return false;
    if (block->span < header_bytes() + prefix_bytes() + 1u) return false;
    return block->span <= heap_end - address;
}

static bool next_link_sane(const PjsHeapBlock *block)
{
    PjsHeapBlock *next = block->next;
    if (next == 0) return block_end(block) == heap_end;
    return block_sane(next) && (uintptr_t)next == block_end(block) &&
           next->previous == block;
}

bool pjs_heap_init(void *base, size_t length)
{
    heap_begin = align_up((uintptr_t)base, PJS_HEAP_MIN_ALIGN);
    uintptr_t supplied_end = (uintptr_t)base + length;
    heap_end = align_down(supplied_end, PJS_HEAP_MIN_ALIGN);
    heap_first = 0;
    heap_cursor = 0;

    size_t minimum = header_bytes() + prefix_bytes() + PJS_HEAP_MIN_PAYLOAD;
    if (heap_end <= heap_begin || heap_end - heap_begin < minimum) return false;

    heap_first = (PjsHeapBlock *)heap_begin;
    *heap_first = (PjsHeapBlock){
        .magic = PJS_HEAP_BLOCK_MAGIC,
        .free = 1u,
        .span = heap_end - heap_begin,
        .previous = 0,
        .next = 0,
    };
    heap_cursor = heap_first;
    return true;
}

static void split_after(PjsHeapBlock *block, uintptr_t split)
{
    if (!next_link_sane(block)) return;
    split = align_up(split, PJS_HEAP_MIN_ALIGN);
    size_t minimum = header_bytes() + prefix_bytes() + PJS_HEAP_MIN_PAYLOAD;
    if (split <= (uintptr_t)block || block_end(block) - split < minimum) return;

    PjsHeapBlock *tail = (PjsHeapBlock *)split;
    *tail = (PjsHeapBlock){
        .magic = PJS_HEAP_BLOCK_MAGIC,
        .free = 1u,
        .span = block_end(block) - split,
        .previous = block,
        .next = block->next,
    };
    if (tail->next != 0) tail->next->previous = tail;
    block->next = tail;
    block->span = split - (uintptr_t)block;
}

void *pjs_heap_alloc(size_t size, size_t alignment)
{
    if (size == 0u) size = 1u;
    if (alignment < PJS_HEAP_MIN_ALIGN) alignment = PJS_HEAP_MIN_ALIGN;
    if (!alignment_valid(alignment) || heap_first == 0) return 0;

    PjsHeapBlock *start = heap_cursor;
    if (start == 0 || !block_sane(start)) start = heap_first;
    PjsHeapBlock *block = start;
    bool wrapped = false;
    for (;;) {
        if (!block_sane(block) || !next_link_sane(block)) return 0;
        if (block->free != 0u) {
            uintptr_t payload_start = (uintptr_t)block + header_bytes();
            uintptr_t user = align_up(payload_start + prefix_bytes(), alignment);
            /* A tail block can be too small even when an earlier block fits.
             * Keep searching through wraparound; never subtract an aligned
             * address beyond this block's end. */
            if (user >= payload_start && user <= block_end(block) &&
                size <= block_end(block) - user) {
                uintptr_t used_end = user + size;
                split_after(block, used_end);
                block->free = 0u;
                PjsHeapPrefix *prefix = (PjsHeapPrefix *)(user - sizeof(PjsHeapPrefix));
                *prefix = (PjsHeapPrefix){
                    .magic = PJS_HEAP_PREFIX_MAGIC,
                    .reserved = 0u,
                    .block = block,
                    .requested = size,
                };
                heap_cursor = block->next != 0 ? block->next : heap_first;
                return (void *)user;
            }
        }
        block = block->next;
        if (block == 0) {
            if (wrapped) return 0;
            wrapped = true;
            block = heap_first;
        }
        if (wrapped && block == start) return 0;
    }
}

static PjsHeapPrefix *prefix_for(void *pointer)
{
    if (pointer == 0) return 0;
    uintptr_t address = (uintptr_t)pointer;
    if (address < heap_begin + sizeof(PjsHeapPrefix) || address >= heap_end) return 0;
    PjsHeapPrefix *prefix = (PjsHeapPrefix *)(address - sizeof(PjsHeapPrefix));
    if (prefix->magic != PJS_HEAP_PREFIX_MAGIC || !block_sane(prefix->block) ||
        prefix->block->free != 0u) return 0;
    return prefix;
}

static void merge_next(PjsHeapBlock *block)
{
    PjsHeapBlock *next = block->next;
    if (next == 0 || !next_link_sane(block) || next->free == 0u) return;
    block->span += next->span;
    block->next = next->next;
    if (block->next != 0) block->next->previous = block;
    if (heap_cursor == next) heap_cursor = block;
    next->magic = 0u;
}

size_t pjs_heap_usable_size(const void *pointer)
{
    PjsHeapPrefix *prefix = prefix_for((void *)pointer);
    return prefix != 0 ? prefix->requested : 0u;
}

void pjs_heap_free(void *pointer)
{
    if (pointer == 0) return;
    PjsHeapPrefix *prefix = prefix_for(pointer);
    if (prefix == 0) return;
    PjsHeapBlock *block = prefix->block;
    prefix->magic = 0u;
    block->free = 1u;
    merge_next(block);
    if (block->previous != 0 && block_sane(block->previous) &&
        block->previous->next == block && block->previous->free != 0u) {
        block = block->previous;
        merge_next(block);
    }
    if (heap_cursor == 0 || !block_sane(heap_cursor)) heap_cursor = block;
}

void *pjs_heap_realloc(void *pointer, size_t new_size, size_t alignment)
{
    if (alignment < PJS_HEAP_MIN_ALIGN) alignment = PJS_HEAP_MIN_ALIGN;
    if (!alignment_valid(alignment)) return 0;
    if (pointer == 0) return pjs_heap_alloc(new_size, alignment);
    if (new_size == 0u) {
        pjs_heap_free(pointer);
        return 0;
    }
    PjsHeapPrefix *prefix = prefix_for(pointer);
    if (prefix == 0) return 0;
    if ((uintptr_t)pointer + new_size <= block_end(prefix->block) &&
        ((uintptr_t)pointer & (alignment > 1u ? alignment - 1u : 0u)) == 0u) {
        prefix->requested = new_size;
        return pointer;
    }

    void *replacement = pjs_heap_alloc(new_size, alignment);
    if (replacement == 0) return 0;
    size_t copy = prefix->requested < new_size ? prefix->requested : new_size;
    uint8_t *out = (uint8_t *)replacement;
    const uint8_t *in = (const uint8_t *)pointer;
    for (size_t index = 0u; index < copy; ++index) out[index] = in[index];
    pjs_heap_free(pointer);
    return replacement;
}

bool pjs_heap_validate(void)
{
    if (heap_first == 0 || heap_begin == 0u || heap_end <= heap_begin) return false;
    uintptr_t expected = heap_begin;
    PjsHeapBlock *previous = 0;
    for (PjsHeapBlock *block = heap_first; block != 0; block = block->next) {
        if (!block_sane(block) || (uintptr_t)block != expected || block->previous != previous) {
            return false;
        }
        if (previous != 0 && previous->free != 0u && block->free != 0u) return false;
        expected = block_end(block);
        previous = block;
    }
    return expected == heap_end;
}

void pjs_heap_stats(PjsHeapStats *stats)
{
    if (stats == 0) return;
    *stats = (PjsHeapStats){0};
    stats->total_bytes = heap_end > heap_begin ? heap_end - heap_begin : 0u;
    for (PjsHeapBlock *block = heap_first; block != 0 && block_sane(block); block = block->next) {
        size_t usable = block->span > header_bytes() ? block->span - header_bytes() : 0u;
        if (block->free != 0u) {
            stats->free_bytes += usable;
            ++stats->free_block_count;
            if (usable > stats->largest_free) stats->largest_free = usable;
        } else {
            stats->allocated_bytes += usable;
            ++stats->allocation_count;
        }
    }
}
