#ifndef POCKETJS_IPOD_PHOTO_CPU_IDLE_H
#define POCKETJS_IPOD_PHOTO_CPU_IDLE_H

#include <stdint.h>

/* Timer2 is owned only during this bounded wait; Timer1 remains the scheduler. */
void cpu_idle_init(void);
void cpu_idle_wait(void);
void cpu_idle_snapshot(uint32_t *window_us, uint32_t *idle_us, uint32_t *waits);

#endif
