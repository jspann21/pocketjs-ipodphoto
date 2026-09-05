#ifndef POCKETJS_IPOD_PHOTO_TIMER_IRQ_H
#define POCKETJS_IPOD_PHOTO_TIMER_IRQ_H

#include <stdint.h>

void timer_irq_init(void);
uint32_t timer_irq_ack_elapsed_us(void);
void timer_irq_stop(void);

#endif
