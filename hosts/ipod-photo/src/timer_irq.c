#include "platform.h"
#include "pp5020.h"
#include "timer_irq.h"

static volatile uint32_t last_irq_us;

void timer_irq_init(void)
{
    PP_CPU_INT_DIS = PP_TIMER1_MASK;
    PP_TIMER1_CFG = 0u;
    (void)PP_TIMER1_VAL; /* clear any inherited pending Timer1 interrupt */
    last_irq_us = PP_USEC_TIMER;
    PP_TIMER1_CFG = 0xc0000000u | (PJS_TIMER_IRQ_US - 1u);
    PP_CPU_INT_EN = PP_TIMER1_MASK;
}

uint32_t timer_irq_ack_elapsed_us(void)
{
    (void)PP_TIMER1_VAL; /* acknowledge Timer1 */
    uint32_t now = PP_USEC_TIMER;
    uint32_t elapsed = now - last_irq_us;
    last_irq_us = now;

    /* A zero delta is possible only when the microsecond counter and interrupt
     * edge are sampled in the same tick. Preserve forward progress without
     * fabricating a large time jump. Unsigned subtraction already handles the
     * normal 32-bit counter wrap. */
    return elapsed == 0u ? 1u : elapsed;
}

void timer_irq_stop(void)
{
    PP_CPU_INT_DIS = PP_TIMER1_MASK;
    PP_TIMER1_CFG = 0u;
}
