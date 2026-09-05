#include "cpu_idle.h"
#include "irq.h"
#include "pp5020.h"

static uint32_t window_start, accumulated_idle, measured_window, measured_idle;
static uint32_t wait_count;
static int initialized;

void cpu_idle_init(void)
{
    PP_CPU_INT_DIS = PP_TIMER2_MASK;
    PP_TIMER2_CFG = 0u;
    (void)PP_TIMER2_VAL;
}

static void sample_window(uint32_t now)
{
    if (!initialized) {
        window_start = now;
        initialized = 1;
    }
    uint32_t elapsed = now - window_start;
    if (elapsed >= 1000000u) {
        measured_window = elapsed;
        measured_idle = accumulated_idle < elapsed ? accumulated_idle : elapsed;
        accumulated_idle = 0u;
        window_start = now;
    }
}

void cpu_idle_wait(void)
{
    sample_window(PP_USEC_TIMER);
    uint32_t irq_state = irq_save_disable();
    /* Never borrow a timer owned by another subsystem or sleep over pending
     * work. IRQ masking closes the check/sleep race; PP5020 wakes on the
     * controller's pending interrupt even with the ARM IRQ mask set, as in
     * Rockbox's core_sleep. COP stays parked and needs no mailbox handshake. */
    if ((PP_TIMER2_CFG & 0x80000000u) != 0u ||
        (PP_CPU_INT_EN_STAT & PP_TIMER2_MASK) != 0u ||
        PP_CPU_INT_STAT != 0u || PP_CPU_HI_INT_STAT != 0u) {
        irq_restore(irq_state);
        return;
    }
    (void)PP_TIMER2_VAL;
    PP_TIMER2_CFG = 0xc0000000u | 999u;
    PP_CPU_INT_EN = PP_TIMER2_MASK;
    uint32_t control = PP_CPU_CTL;
    uint32_t started = PP_USEC_TIMER;
    PP_CPU_CTL = PP_PROC_SLEEP;
    PP_CPU_CTL = control & ~PP_PROC_SLEEP;
    uint32_t elapsed = PP_USEC_TIMER - started;
    PP_CPU_INT_DIS = PP_TIMER2_MASK;
    PP_TIMER2_CFG = 0u;
    (void)PP_TIMER2_VAL;
    irq_restore(irq_state);
    accumulated_idle += elapsed;
    ++wait_count;
}

void cpu_idle_snapshot(uint32_t *window_us, uint32_t *idle_us, uint32_t *waits)
{
    sample_window(PP_USEC_TIMER);
    *window_us = measured_window;
    *idle_us = measured_idle;
    *waits = wait_count;
}
