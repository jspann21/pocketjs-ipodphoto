#include "pp5020.h"
#include "timer.h"

uint32_t timer_now_us(void)
{
    return PP_USEC_TIMER;
}

void timer_delay_us(uint32_t delay)
{
    uint32_t start = timer_now_us();
    while ((uint32_t)(timer_now_us() - start) < delay) {
        __asm__ volatile("nop");
    }
}

void timer_wait_until(uint32_t deadline)
{
    while ((int32_t)(timer_now_us() - deadline) < 0) {
        __asm__ volatile("nop");
    }
}
