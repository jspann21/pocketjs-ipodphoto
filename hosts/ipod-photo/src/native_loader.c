#include "native_loader.h"

#include "cache.h"
#include "irq.h"
#include "platform.h"
#include "pp5020.h"

#include <stddef.h>
#include <stdint.h>

#define PJS_NATIVE_IMAGE_MIN_BYTES 32u
#define PJS_NATIVE_IMAGE_MAX_BYTES (16u * 1024u * 1024u)
#define PJS_NATIVE_COP_WAIT_ITERS 1000000u
#define PJS_ARM_BRANCH_MASK 0x0f000000u
#define PJS_ARM_BRANCH_OPCODE 0x0a000000u

extern const uint32_t pjs_chainload_stub_start[];
extern const uint32_t pjs_chainload_stub_end[];

typedef void (*PjsChainloadStub)(const uint8_t *source, uint32_t length)
    __attribute__((noreturn));

bool pjs_native_image_admissible(const uint8_t *image, uint32_t length)
{
    if (image == 0 || length < PJS_NATIVE_IMAGE_MIN_BYTES ||
        length > PJS_NATIVE_IMAGE_MAX_BYTES || (length & 3u) != 0u) {
        return false;
    }

    uintptr_t source = (uintptr_t)image;
    /* The IRAM stub performs word loads; reject an unaligned caller pointer
     * before that pointer can reach the ARM7 data bus. */
    if ((source & 3u) != 0u || source >= PJS_SDRAM_BYTES ||
        length > PJS_SDRAM_BYTES - source) {
        return false;
    }

    /* Both supported PocketJS entry forms begin with an unconditional ARM
     * branch from vector zero. The uploaded frame CRC protects the remaining
     * bytes; this check prevents jumping into an arbitrary data buffer. */
    uint32_t reset_vector = (uint32_t)image[0] |
        ((uint32_t)image[1] << 8) |
        ((uint32_t)image[2] << 16) |
        ((uint32_t)image[3] << 24);
    if ((reset_vector & PJS_ARM_BRANCH_MASK) != PJS_ARM_BRANCH_OPCODE) {
        return false;
    }

    /* A flat image is copied to address zero.  Keep the reset branch inside
     * that image so a valid CRC cannot turn CHAINLOAD into an arbitrary jump
     * through the current firmware's address space. ARM B uses PC=8 at the
     * vector and a signed 24-bit displacement measured in words. */
    int32_t branch_words = (int32_t)(reset_vector & 0x00ffffffu);
    if ((branch_words & 0x00800000) != 0) {
        branch_words -= 0x01000000;
    }
    int32_t branch_target = 8 + branch_words * 4;
    return branch_target >= 0 && (uint32_t)branch_target < length;
}

void pjs_native_image_chainload(const uint8_t *image, uint32_t length)
{
    if (!pjs_native_image_admissible(image, length)) {
        pp_reboot();
    }

    irq_disable_global();
    PP_CPU_INT_DIS = 0xffffffffu;
    PP_CPU_HI_INT_DIS = 0xffffffffu;
    PP_COP_INT_DIS = 0xffffffffu;
    PP_COP_HI_INT_DIS = 0xffffffffu;
    PP_INT_FORCED_CLR = 0xffffffffu;
    PP_HI_INT_FORCED_CLR = 0xffffffffu;

    /* crt0 requires the COP sleep invariant before it will remap SDRAM. The
     * normal firmware keeps the COP parked, but a native handoff must enforce
     * that boundary rather than inheriting it by assumption. */
    PP_COP_CTL = PP_PROC_SLEEP;
    uint32_t cop_wait = PJS_NATIVE_COP_WAIT_ITERS;
    while ((PP_COP_CTL & PP_PROC_SLEEP) == 0u && cop_wait != 0u) {
        --cop_wait;
    }
    if (cop_wait == 0u) pp_reboot();

    /* Commit every uploaded byte to SDRAM before executing outside SDRAM.
     * The reset path of the new image independently establishes cache state. */
    cache_take_ownership_disabled();

    uintptr_t stub_bytes = (uintptr_t)pjs_chainload_stub_end -
        (uintptr_t)pjs_chainload_stub_start;
    if (stub_bytes == 0u || stub_bytes > 256u || (stub_bytes & 3u) != 0u) {
        pp_reboot();
    }

    volatile uint32_t *destination =
        (volatile uint32_t *)(uintptr_t)PJS_IRAM_BASE;
    for (uintptr_t index = 0u; index < stub_bytes / 4u; ++index) {
        destination[index] = pjs_chainload_stub_start[index];
    }
    __asm__ volatile("nop\n\tnop\n\tnop\n\tnop" ::: "memory");

    ((PjsChainloadStub)(uintptr_t)PJS_IRAM_BASE)(image, length);
    __builtin_unreachable();
}
