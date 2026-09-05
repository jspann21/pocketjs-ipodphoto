#include "irq.h"
#include "panic.h"
#include "platform.h"
#include "pp5020.h"
#include "scheduler.h"
#include "timer_irq.h"


#include "audio_dma.h"

static volatile uint32_t unexpected_low;
static volatile uint32_t unexpected_high;

uint32_t irq_save_disable(void)
{
    uint32_t cpsr;
    uint32_t masked;
    __asm__ volatile(
        "mrs %0, cpsr\n\t"
        "orr %1, %0, #0x80\n\t"
        "msr cpsr_c, %1"
        : "=r"(cpsr), "=r"(masked)
        :
        : "memory", "cc");
    return cpsr;
}

void irq_restore(uint32_t saved_cpsr)
{
    __asm__ volatile("msr cpsr_c, %0" : : "r"(saved_cpsr) : "memory", "cc");
}

void irq_enable_global(void)
{
    uint32_t cpsr;
    __asm__ volatile(
        "mrs %0, cpsr\n\t"
        "bic %0, %0, #0x80\n\t"
        "msr cpsr_c, %0"
        : "=&r"(cpsr)
        :
        : "memory", "cc");
}

void irq_disable_global(void)
{
    (void)irq_save_disable();
}

void irq_dispatch(void)
{
    uint32_t low = PP_CPU_INT_STAT;
    uint32_t high = PP_CPU_HI_INT_STAT;

    if ((low & PP_TIMER1_MASK) != 0u) {
        /* Account real elapsed microseconds rather than assuming one exact
         * millisecond per interrupt. This keeps fixed-step time correct even
         * if an inherited clock source changes the timer cadence. */
        scheduler_irq_advance_us(timer_irq_ack_elapsed_us());
        low &= ~PP_TIMER1_MASK;
    }

    if ((low & PP_DMA_MASK) != 0u) {
        /* DMA status is acknowledged by the owner. Keep this path separate
         * from the legacy unexpected-interrupt latch: a DMA completion is a
         * normal audio clock event, while a foreign/stale completion is
         * fail-closed by pjs_audio_dma_irq(). */
        (void)pjs_audio_dma_irq();
        low &= ~PP_DMA_MASK;
    }

    if (low != 0u) {
        unexpected_low |= low;
        PP_CPU_INT_DIS = low;
    }
    if (high != 0u) {
        unexpected_high |= high;
        PP_CPU_HI_INT_DIS = high;
    }
}

uint32_t irq_unexpected_low(void)
{
    return unexpected_low;
}

uint32_t irq_unexpected_high(void)
{
    return unexpected_high;
}
