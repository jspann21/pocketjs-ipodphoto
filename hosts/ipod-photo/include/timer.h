#ifndef POCKETJS_IPOD_PHOTO_TIMER_H
#define POCKETJS_IPOD_PHOTO_TIMER_H

#include <stdint.h>

uint32_t timer_now_us(void);
void timer_delay_us(uint32_t delay);
void timer_wait_until(uint32_t deadline);

#endif
