#include "cache.h"
#include "platform.h"
#include "pp5020.h"

#include <stdint.h>

/* PP5020 cache setup used by the established A1099 firmware support. Low
 * remapped SDRAM is cached; the native 0x10000000 window remains uncached. */
#define PJS_CACHE_MATCH 0x00001c00u
#define PJS_CACHE_OPERATION_BASE 0x00000fc0u
#define PJS_CACHE_CPU_PRIORITY 0x00000010u
#define PJS_CACHE_PRIME_ADDRESS 0x00002000u
#define PJS_CACHE_PROBE_WORDS 16u
#define PJS_CACHE_PROBE_SEED 0xa1090000u

static bool owned_enabled;
static uint32_t cache_probe[PJS_CACHE_PROBE_WORDS]
    __attribute__((aligned(PP_CACHE_LINE_BYTES)));

static bool cache_wait_idle(void)
{
    uint32_t remaining = PJS_CACHE_POLL_LIMIT;
    while ((PP_CACHE_CTL & PP_CACHE_CTL_BUSY) != 0u && remaining != 0u) {
        --remaining;
        __asm__ volatile("nop");
    }
    return remaining != 0u;
}

static bool cache_clean_all(void)
{
    if (!owned_enabled) return true;
    __asm__ volatile("" ::: "memory");
    PP_CACHE_OPERATION |= PP_CACHE_OP_FLUSH;
    if (!cache_wait_idle()) return false;
    __asm__ volatile("nop\n\tnop\n\tnop\n\tnop" ::: "memory");
    return true;
}

static void cache_emergency_reset(void) __attribute__((noreturn));
static void cache_emergency_reset(void)
{
    /* If writeback cannot complete, neither a C panic path nor disabling the
     * cache is safe: the active SDRAM stack may contain dirty state. */
    PP_DEV_RS |= PP_DEV_SYSTEM;
    for (;;) __asm__ volatile("nop");
}

static void cache_disable_after_clean(void)
{
    PP_CACHE_CTL = 0u;
    pp_nop3();
    owned_enabled = false;
}

void cache_take_ownership_disabled(void)
{
    if ((PP_CACHE_CTL & PP_CACHE_CTL_ENABLE) != 0u) {
        owned_enabled = true;
        if (!cache_clean_all()) cache_emergency_reset();
    }
    cache_disable_after_clean();
}

static bool __attribute__((used, noinline)) cache_enable_and_test_inner(void)
{
    cache_take_ownership_disabled();

    PP_CACHE_CTL = PP_CACHE_CTL_INIT;
    PP_CACHE_PRIORITY |= PJS_CACHE_CPU_PRIORITY;
    PP_CACHE_MASK = PJS_CACHE_MATCH;
    PP_CACHE_OPERATION = PJS_CACHE_OPERATION_BASE;
    PP_CACHE_CTL = PP_CACHE_CTL_INIT | PP_CACHE_CTL_ENABLE | PP_CACHE_CTL_RUN;
    __asm__ volatile("nop\n\tnop\n\tnop\n\tnop" ::: "memory");

    /* Fill every line before the first whole-cache clean. PP5020 invalid cache
     * status entries are known to cause corruption under mixed fetch/write
     * traffic, so this step is part of the ownership contract. */
    volatile const uint8_t *prime =
        (volatile const uint8_t *)(uintptr_t)PJS_CACHE_PRIME_ADDRESS;
    for (uint32_t offset = 0u; offset < PP_CACHE_BYTES;
         offset += PP_CACHE_LINE_BYTES) {
        (void)prime[offset];
    }
    owned_enabled = true;

    uintptr_t probe_address = (uintptr_t)cache_probe;
    if (probe_address >= PJS_SDRAM_BYTES) {
        if (!cache_clean_all()) cache_emergency_reset();
        cache_disable_after_clean();
        return false;
    }

    /* Prove cached writeback through the native uncached SDRAM alias before
     * PocketJS allocates or renders anything. A mismatch is recoverable because
     * the successful full clean makes it safe to return to uncached operation. */
    for (uint32_t index = 0u; index < PJS_CACHE_PROBE_WORDS; ++index) {
        cache_probe[index] = PJS_CACHE_PROBE_SEED ^ (index * 0x01010101u);
    }
    if (!cache_clean_all()) cache_emergency_reset();

    volatile const uint32_t *uncached =
        (volatile const uint32_t *)(uintptr_t)(PP_NOCACHE_BASE | probe_address);
    bool matches = true;
    for (uint32_t index = 0u; index < PJS_CACHE_PROBE_WORDS; ++index) {
        uint32_t expected = PJS_CACHE_PROBE_SEED ^ (index * 0x01010101u);
        if (uncached[index] != expected) {
            matches = false;
            break;
        }
    }

    for (uint32_t index = 0u; index < PJS_CACHE_PROBE_WORDS; ++index) {
        cache_probe[index] = 0u;
    }
    if (!cache_clean_all()) cache_emergency_reset();

    if (!matches) {
        cache_disable_after_clean();
        return false;
    }
    return true;
}

/* The enable/self-test/fallback transaction uses an IRAM stack. This prevents
 * a recoverable cache mismatch from depending on dirty SDRAM stack contents
 * while the cache is being cleaned and potentially disabled. Interrupts are
 * still globally masked when this function runs. */
bool __attribute__((naked)) cache_take_ownership_enabled(void)
{
    __asm__ volatile(
        "ldr r2, =0x40017ff8\n"
        "str sp, [r2]\n"
        "str lr, [r2, #4]\n"
        "ldr sp, =0x40017ff0\n"
        "bl cache_enable_and_test_inner\n"
        "ldr r2, =0x40017ff8\n"
        "ldr sp, [r2]\n"
        "ldr lr, [r2, #4]\n"
        "bx lr\n");
}

bool cache_owned_enabled(void)
{
    return owned_enabled && (PP_CACHE_CTL & PP_CACHE_CTL_ENABLE) != 0u;
}

void cache_clean_range(const void *address, size_t length)
{
    (void)address;
    (void)length;
    /* PP5020 has no qualified deterministic range-clean primitive. The cache
     * is only 8 KiB, so the conservative API performs a complete writeback. */
    if (!cache_clean_all()) cache_emergency_reset();
}
