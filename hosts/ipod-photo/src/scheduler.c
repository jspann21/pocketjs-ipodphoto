#include "irq.h"
#include "scheduler.h"

#define PJS_PHASE_DENOMINATOR 1000000u

static PjsScheduler global_scheduler;

void scheduler_reset(PjsScheduler *scheduler)
{
    if (scheduler == 0) return;
    *scheduler = (PjsScheduler){0};
}

void scheduler_advance_us(PjsScheduler *scheduler, uint32_t elapsed_us)
{
    if (scheduler == 0 || elapsed_us == 0u) return;

    scheduler->elapsed_us += elapsed_us;
    uint32_t produced = 0u;
    while (elapsed_us != 0u) {
        uint32_t chunk = elapsed_us > PJS_PHASE_DENOMINATOR
                             ? PJS_PHASE_DENOMINATOR
                             : elapsed_us;
        elapsed_us -= chunk;
        scheduler->phase += chunk * PJS_FIXED_TICK_HZ;
        while (scheduler->phase >= PJS_PHASE_DENOMINATOR) {
            scheduler->phase -= PJS_PHASE_DENOMINATOR;
            ++produced;
        }
    }
    scheduler->total_ticks += produced;

    uint32_t room = PJS_SCHEDULER_MAX_PENDING - scheduler->pending;
    uint32_t accepted = produced < room ? produced : room;
    scheduler->pending += accepted;
    scheduler->dropped_ticks += produced - accepted;
}

uint32_t scheduler_take_batch(PjsScheduler *scheduler, uint32_t maximum)
{
    if (scheduler == 0 || maximum == 0u || scheduler->pending == 0u) return 0u;
    uint32_t count = scheduler->pending < maximum ? scheduler->pending : maximum;
    scheduler->pending -= count;
    return count;
}

bool scheduler_take(PjsScheduler *scheduler)
{
    return scheduler_take_batch(scheduler, 1u) != 0u;
}

void scheduler_global_reset(void)
{
    uint32_t state = irq_save_disable();
    scheduler_reset(&global_scheduler);
    irq_restore(state);
}

void scheduler_irq_advance_us(uint32_t elapsed_us)
{
    scheduler_advance_us(&global_scheduler, elapsed_us);
}

uint32_t scheduler_take_fixed_batch(uint32_t maximum)
{
    uint32_t state = irq_save_disable();
    uint32_t result = scheduler_take_batch(&global_scheduler, maximum);
    irq_restore(state);
    return result;
}

bool scheduler_take_fixed_tick(void)
{
    return scheduler_take_fixed_batch(1u) != 0u;
}

void scheduler_snapshot(PjsScheduler *out)
{
    if (out == 0) return;
    uint32_t state = irq_save_disable();
    *out = global_scheduler;
    irq_restore(state);
}
