#ifndef POCKETJS_IPOD_PHOTO_IRQ_H
#define POCKETJS_IPOD_PHOTO_IRQ_H

#include <stdint.h>

uint32_t irq_save_disable(void);
void irq_restore(uint32_t saved_cpsr);
void irq_enable_global(void);
void irq_disable_global(void);
void irq_dispatch(void);
uint32_t irq_unexpected_low(void);
uint32_t irq_unexpected_high(void);

#endif
