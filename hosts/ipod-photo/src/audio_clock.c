#include "audio_clock.h"
#include "pp5020.h"
#include <stdbool.h>
#include <stddef.h>

#define PLL_CONTROL MMIO32(0x60006034u)
#define DEV_TIMING1 MMIO32(0x70000034u)
#define INIT_PLL 0x40000000u
#define BOOST_PLL 0x8a020a03u
#define BOOST_TIMING 0x00000808u
#define BOOST_SOURCE 0x20007777u

/* Reserve 512 bytes at 0x40010000: above all exception stacks (top
 * 0x40008000), below the reset SVC stack and cache scratch at 0x40017c00+.
 * The live main stack is in SDRAM. CPU startup and audio share this slot. */
#define CLOCK_IRAM 0x40010000u
#define CLOCK_IRAM_BYTES 512u
extern const uint32_t pjs_audio_clock_stub_start[], pjs_audio_clock_stub_end[];
typedef void (*ClockTransition)(uint32_t, uint32_t, uint32_t, uint32_t);
static struct { uint32_t pll, timing, source, init; bool owned; } saved;

static bool quiescent(void)
{
    return (PP_COP_CTL & PP_PROC_SLEEP) != 0u &&
           (PP_DMA0_STATUS & PP_DMA_STATUS_BUSY) == 0u &&
           (PP_DMA0_CMD & PP_DMA_CMD_START) == 0u;
}

static void transition(uint32_t pll, uint32_t timing, uint32_t source, uint32_t init)
{
    ((ClockTransition)(uintptr_t)CLOCK_IRAM)(pll, timing, source, init);
}

static bool clock_matches(uint32_t pll, uint32_t timing,
                          uint32_t source, uint32_t init)
{
    return PLL_CONTROL == pll && DEV_TIMING1 == timing &&
           PP_CLOCK_SOURCE == source && (PP_DEV_INIT2 & INIT_PLL) == init;
}

static int prepare_transition(void)
{
    size_t bytes = (uintptr_t)pjs_audio_clock_stub_end -
                   (uintptr_t)pjs_audio_clock_stub_start;
    if (bytes == 0u || bytes > CLOCK_IRAM_BYTES || (bytes & 3u) != 0u) return -1;
    volatile uint32_t *iram = (volatile uint32_t *)(uintptr_t)CLOCK_IRAM;
    for (size_t i = 0u; i < bytes / 4u; ++i) iram[i] = pjs_audio_clock_stub_start[i];
    for (size_t i = 0u; i < bytes / 4u; ++i)
        if (iram[i] != pjs_audio_clock_stub_start[i]) return -1;
    __asm__ volatile("" ::: "memory");
    return 0;
}

int pjs_cpu_clock_init(void)
{
    if (saved.owned || !quiescent() || prepare_transition() != 0) return -1;
    if (!clock_matches(BOOST_PLL, BOOST_TIMING, BOOST_SOURCE, INIT_PLL))
        transition(BOOST_PLL, BOOST_TIMING, BOOST_SOURCE, INIT_PLL);
    return clock_matches(BOOST_PLL, BOOST_TIMING, BOOST_SOURCE, INIT_PLL) ? 0 : -1;
}

int pjs_audio_clock_acquire(void)
{
    if (saved.owned || !quiescent() || prepare_transition() != 0) return -1;
    saved.pll = PLL_CONTROL;
    saved.timing = DEV_TIMING1;
    saved.source = PP_CLOCK_SOURCE;
    saved.init = PP_DEV_INIT2 & INIT_PLL;
    saved.owned = true;
    if (!clock_matches(BOOST_PLL, BOOST_TIMING, BOOST_SOURCE, INIT_PLL))
        transition(BOOST_PLL, BOOST_TIMING, BOOST_SOURCE, INIT_PLL);
    return clock_matches(BOOST_PLL, BOOST_TIMING, BOOST_SOURCE, INIT_PLL) ? 0 : -1;
}

int pjs_audio_clock_release(void)
{
    if (!saved.owned) return 0;
    if (!quiescent()) return -1;
    if (!clock_matches(saved.pll, saved.timing, saved.source, saved.init))
        transition(saved.pll, saved.timing, saved.source, saved.init);
    if (!clock_matches(saved.pll, saved.timing, saved.source, saved.init))
        return -1;
    saved.owned = false;
    return 0;
}
