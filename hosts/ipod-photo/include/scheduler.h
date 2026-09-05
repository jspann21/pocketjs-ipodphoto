#ifndef POCKETJS_IPOD_PHOTO_SCHEDULER_H
#define POCKETJS_IPOD_PHOTO_SCHEDULER_H

#include <stdbool.h>
#include <stdint.h>

#define PJS_FIXED_TICK_HZ 60u
#define PJS_SCHEDULER_MAX_PENDING 256u
#define PJS_SCHEDULER_MAX_STEPS_PER_PASS 32u

typedef struct {
    uint32_t phase;
    uint32_t pending;
    uint32_t total_ticks;
    uint32_t dropped_ticks;
    uint32_t elapsed_us;
} PjsScheduler;

void scheduler_reset(PjsScheduler *scheduler);
void scheduler_advance_us(PjsScheduler *scheduler, uint32_t elapsed_us);
bool scheduler_take(PjsScheduler *scheduler);
uint32_t scheduler_take_batch(PjsScheduler *scheduler, uint32_t maximum);

void scheduler_global_reset(void);
void scheduler_irq_advance_us(uint32_t elapsed_us);
bool scheduler_take_fixed_tick(void);
uint32_t scheduler_take_fixed_batch(uint32_t maximum);
void scheduler_snapshot(PjsScheduler *out);

#endif
