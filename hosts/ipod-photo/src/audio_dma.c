#include "audio_dma.h"

#include <stdbool.h>
#include <stdint.h>

#include "irq.h"
#include "pp5020.h"
#include "timer.h"

/* A DMA command is deliberately short enough that an IRQ can rearm the next
 * segment promptly. The FIFO request, rather than Timer1, is the native
 * 44.1-kHz clock. */
#define PJS_AUDIO_DMA_PLAY_CONFIG \
    ((PP_DMA_REQ_IIS << PP_DMA_CMD_REQ_ID_POS) | PP_DMA_CMD_RAM_TO_PER | \
     PP_DMA_CMD_SINGLE | PP_DMA_CMD_WAIT_REQ | PP_DMA_CMD_INTR)

#define PJS_AUDIO_DMA_STOP_TIMEOUT_US 100000u
#define PJS_AUDIO_DMA_WATCHDOG_US      250000u
#define PJS_AUDIO_DMA_DRAIN_TIMEOUT_US 100000u
#define PJS_AUDIO_DMA_RING_MASK        (PJS_AUDIO_DMA_RING_FRAMES - 1u)

/* These live in normal .bss, but every CPU access below uses the PP5020's
 * uncached SDRAM alias. Consequently a DMA transfer never observes a stale
 * cached producer line and no cache-wide clean is needed in the IRQ path. */
static uint32_t audio_dma_ring[PJS_AUDIO_DMA_RING_FRAMES]
    __attribute__((aligned(32), section(".bss.audio_dma_uncached")));
static uint32_t audio_dma_silence[PJS_AUDIO_DMA_CHUNK_FRAMES]
    __attribute__((aligned(32), section(".bss.audio_dma_uncached")));

typedef struct {
    bool initialized;
    bool dma_active;
    bool in_flight_real;
    bool pause_requested;
    bool drain_pending;
    bool starved;
    bool playing;
    bool paused;
    bool ended;
    bool end_requested;
    bool cleanup_pending;
    bool hardware_probe_done;
    uint8_t fault;
    uint32_t in_flight_frames;
    uint32_t submitted_frames;
    uint32_t consumed_frames;
    uint32_t underruns;
    uint32_t silence_frames;
    uint32_t irq_count;
    uint32_t dma_restarts;
    uint32_t watchdog_faults;
    uint32_t stop_timeouts;
    uint32_t drain_timeouts;
    uint32_t end_requests;
    uint32_t end_events;
    uint32_t last_error;
    uint32_t last_irq_us;
    uint32_t drain_started_us;
} PjsAudioDmaRuntime;

static volatile uint32_t audio_dma_write_pos;
static volatile uint32_t audio_dma_read_pos;
static volatile PjsAudioDmaRuntime audio_dma;

static volatile uint32_t *audio_dma_ring_uncached(void)
{
    uintptr_t address = (uintptr_t)audio_dma_ring;
    if (address < (uintptr_t)PP_NOCACHE_BASE) {
        address |= (uintptr_t)PP_NOCACHE_BASE;
    }
    return (volatile uint32_t *)address;
}

static volatile uint32_t *audio_dma_silence_uncached(void)
{
    uintptr_t address = (uintptr_t)audio_dma_silence;
    if (address < (uintptr_t)PP_NOCACHE_BASE) {
        address |= (uintptr_t)PP_NOCACHE_BASE;
    }
    return (volatile uint32_t *)address;
}

static uint32_t audio_dma_uncached_address(const volatile uint32_t *address)
{
    uintptr_t value = (uintptr_t)address;
    if (value < (uintptr_t)PP_NOCACHE_BASE) {
        value |= (uintptr_t)PP_NOCACHE_BASE;
    }
    return (uint32_t)value;
}

static void audio_dma_latch_fault(uint32_t fault)
{
    if (audio_dma.fault == PJS_AUDIO_DMA_FAULT_NONE) {
        audio_dma.fault = (uint8_t)fault;
    }
    audio_dma.last_error = fault;
}

static bool audio_dma_ring_occupancy(uint32_t *queued)
{
    uint32_t write_pos = audio_dma_write_pos;
    uint32_t read_pos = audio_dma_read_pos;
    uint32_t count = write_pos - read_pos;
    if (count > PJS_AUDIO_DMA_RING_FRAMES) {
        audio_dma_latch_fault(PJS_AUDIO_DMA_FAULT_RING);
        if (queued != 0) *queued = 0u;
        return false;
    }
    if (queued != 0) *queued = count;
    return true;
}

static void audio_dma_disable_irq(void)
{
    PP_CPU_INT_DIS = PP_DMA_MASK;
}

static void audio_dma_enable_irq(void)
{
    /* A clear priority bit selects the ordinary IRQ vector. Rockbox's audio
     * path uses FIQ, but this gate intentionally exercises the C IRQ ABI. */
    PP_CPU_INT_PRIORITY &= ~PP_DMA_MASK;
    PP_CPU_INT_EN = PP_DMA_MASK;
}

static void audio_dma_clear_fifo(void)
{
    PP_IISCONFIG &= ~PP_IIS_TXFIFOEN;
    PP_IISFIFO_CFG |= PP_IIS_TXCLR;
}

/* Stop any inherited channel-0 transaction before this gate clears the FIFO
 * or reuses the static ring. This has no dependency on runtime state, so it
 * is safe during the first init after a bootloader handoff. */
static int audio_dma_quiesce_hardware(void)
{
    PP_DMA_REQ_STATUS &= ~PP_DMA_REQ_IIS_MASK;
    uint32_t command = PP_DMA0_CMD;
    PP_DMA0_CMD = command & ~(PP_DMA_CMD_START | PP_DMA_CMD_INTR);
    __asm__ volatile("" ::: "memory");

    uint32_t deadline = timer_now_us() + PJS_AUDIO_DMA_STOP_TIMEOUT_US;
    for (;;) {
        uint32_t status = PP_DMA0_STATUS;
        if ((status & (PP_DMA_STATUS_BUSY | PP_DMA_STATUS_INTR)) == 0u) {
            (void)PP_DMA0_STATUS;
            return PJS_AUDIO_DMA_RESULT_OK;
        }
        if ((int32_t)(timer_now_us() - deadline) >= 0) break;
    }
    (void)PP_DMA0_STATUS;
    return PJS_AUDIO_DMA_RESULT_TIMEOUT;
}

static int audio_dma_stop_hardware(bool flush_fifo)
{
    audio_dma_disable_irq();
    if (audio_dma_quiesce_hardware() != PJS_AUDIO_DMA_RESULT_OK) {
        if (audio_dma.stop_timeouts != UINT32_MAX) ++audio_dma.stop_timeouts;
        audio_dma.cleanup_pending = true;
        audio_dma_latch_fault(PJS_AUDIO_DMA_FAULT_STOP_TIMEOUT);
        audio_dma.playing = false;
        audio_dma.paused = false;
        audio_dma.pause_requested = false;
        return PJS_AUDIO_DMA_RESULT_TIMEOUT;
    }

    audio_dma.dma_active = false;
    audio_dma.in_flight_frames = 0u;
    audio_dma.in_flight_real = false;
    audio_dma.drain_pending = false;
    audio_dma.cleanup_pending = false;
    audio_dma.hardware_probe_done = true;
    PP_DMA_REQ_STATUS &= ~PP_DMA_REQ_IIS_MASK;
    if (flush_fifo) audio_dma_clear_fifo();
    return PJS_AUDIO_DMA_RESULT_OK;
}

static bool audio_dma_arm(volatile uint32_t *source, uint32_t frames,
                          bool real)
{
    if (frames == 0u || frames > PJS_AUDIO_DMA_CHUNK_FRAMES) return false;

    uint32_t bytes = frames * 4u;
    if (bytes > PP_DMA_CMD_SIZE_MASK) return false;

    /* Publish all metadata before START: a request can be serviced as soon as
     * the command reaches the DMA block. */
    audio_dma.in_flight_frames = frames;
    audio_dma.in_flight_real = real;
    audio_dma.dma_active = true;
    if (real) audio_dma.submitted_frames += frames;
    if (audio_dma.dma_restarts != UINT32_MAX) ++audio_dma.dma_restarts;
    __asm__ volatile("" ::: "memory");

    PP_DMA0_RAM_ADDR = audio_dma_uncached_address(source);
    PP_DMA0_CMD = PJS_AUDIO_DMA_PLAY_CONFIG | (bytes - 4u) |
                  PP_DMA_CMD_START;
    return true;
}

/* Called with IRQs disabled, either from the main context or from the DMA
 * handler. It never calls out to codec, guest, allocation, or cache code. */
static void audio_dma_start_next(void)
{
    if (!audio_dma.initialized || audio_dma.fault != 0u ||
        !audio_dma.playing || audio_dma.dma_active ||
        audio_dma.drain_pending) {
        return;
    }

    uint32_t queued = 0u;
    if (!audio_dma_ring_occupancy(&queued)) return;

    if (queued != 0u) {
        uint32_t read_pos = audio_dma_read_pos;
        uint32_t contiguous = PJS_AUDIO_DMA_RING_FRAMES -
                              (read_pos & PJS_AUDIO_DMA_RING_MASK);
        uint32_t frames = queued;
        if (frames > contiguous) frames = contiguous;
        if (frames > PJS_AUDIO_DMA_CHUNK_FRAMES) {
            frames = PJS_AUDIO_DMA_CHUNK_FRAMES;
        }
        if (!audio_dma_arm(&audio_dma_ring_uncached()[read_pos &
                                                     PJS_AUDIO_DMA_RING_MASK],
                           frames, true)) {
            audio_dma_latch_fault(PJS_AUDIO_DMA_FAULT_RING);
            audio_dma.playing = false;
            return;
        }
        audio_dma.starved = false;
        return;
    }

    /* End is a two-stage fact: all submitted source frames must have retired,
     * then the hardware FIFO must drain. A final silence DMA block is allowed
     * to complete before the latter check, so no in-flight source frame is
     * mistaken for an ended stream. */
    if (audio_dma.end_requested &&
        audio_dma.submitted_frames == audio_dma.consumed_frames) {
        audio_dma.drain_pending = true;
        audio_dma.playing = false;
        audio_dma.paused = false;
        audio_dma.drain_started_us = timer_now_us();
        return;
    }

    if (!audio_dma.starved) {
        audio_dma.starved = true;
        if (audio_dma.underruns != UINT32_MAX) ++audio_dma.underruns;
    }
    if (!audio_dma_arm(audio_dma_silence_uncached(),
                       PJS_AUDIO_DMA_CHUNK_FRAMES, false)) {
        audio_dma_latch_fault(PJS_AUDIO_DMA_FAULT_RING);
        audio_dma.playing = false;
    }
}

static void audio_dma_flush_ring(void)
{
    audio_dma_write_pos = audio_dma_read_pos;
    audio_dma.pause_requested = false;
    audio_dma.starved = false;
    audio_dma.drain_pending = false;
    audio_dma.ended = false;
}

static void audio_dma_finish_end(uint32_t now)
{
    if (!audio_dma.drain_pending) return;
    if (PP_IIS_TX_IS_EMPTY) {
        audio_dma_clear_fifo();
        PP_DMA_REQ_STATUS &= ~PP_DMA_REQ_IIS_MASK;
        audio_dma.drain_pending = false;
        audio_dma.ended = true;
        audio_dma.starved = false;
        if (audio_dma.end_events != UINT32_MAX) ++audio_dma.end_events;
        return;
    }
    if ((uint32_t)(now - audio_dma.drain_started_us) >=
        PJS_AUDIO_DMA_DRAIN_TIMEOUT_US) {
        if (audio_dma.drain_timeouts != UINT32_MAX) {
            ++audio_dma.drain_timeouts;
        }
        audio_dma_latch_fault(PJS_AUDIO_DMA_FAULT_DRAIN_TIMEOUT);
        audio_dma.drain_pending = false;
        audio_dma.playing = false;
    }
}

int pjs_audio_dma_init(void)
{
    uint32_t saved = irq_save_disable();

    if (audio_dma.initialized) {
        int stop_result = audio_dma_stop_hardware(true);
        if (stop_result != PJS_AUDIO_DMA_RESULT_OK) {
            irq_restore(saved);
            return stop_result;
        }
    } else {
        /* The bootloader or an earlier owner may have left DMA0 configured;
         * never overwrite its source ring until the channel is quiescent. */
        audio_dma_disable_irq();
        if (audio_dma_quiesce_hardware() != PJS_AUDIO_DMA_RESULT_OK) {
            audio_dma.cleanup_pending = true;
            audio_dma_latch_fault(PJS_AUDIO_DMA_FAULT_STOP_TIMEOUT);
            irq_restore(saved);
            return PJS_AUDIO_DMA_RESULT_TIMEOUT;
        }
        audio_dma.cleanup_pending = false;
        audio_dma.hardware_probe_done = true;
    }

    audio_dma_disable_irq();
    audio_dma_clear_fifo();
    PP_DMA_MASTER_CONTROL |= PP_DMA_MASTER_ENABLE;
    PP_DMA_REQ_STATUS |= PP_DMA_REQ_IIS_MASK;
    PP_DMA0_PER_ADDR = (uint32_t)(uintptr_t)&PP_IISFIFO_WR;
    PP_DMA0_FLAGS = PP_DMA_FLAGS_UNK26;
    PP_DMA0_INCR = PP_DMA_INCR_RANGE_FIXED | PP_DMA_INCR_WIDTH_32BIT;

    volatile uint32_t *ring = audio_dma_ring_uncached();
    for (uint32_t index = 0u; index < PJS_AUDIO_DMA_RING_FRAMES; ++index) {
        ring[index] = 0u;
    }
    volatile uint32_t *silence = audio_dma_silence_uncached();
    for (uint32_t index = 0u; index < PJS_AUDIO_DMA_CHUNK_FRAMES; ++index) {
        silence[index] = 0u;
    }

    audio_dma_write_pos = 0u;
    audio_dma_read_pos = 0u;
    audio_dma = (PjsAudioDmaRuntime){
        .initialized = true,
    };
    irq_restore(saved);
    return PJS_AUDIO_DMA_RESULT_OK;
}

uint32_t pjs_audio_dma_write(const int16_t *interleaved, uint32_t frames)
{
    if (interleaved == 0 || frames == 0u) return 0u;
    if (!audio_dma.initialized || audio_dma.fault != 0u ||
        audio_dma.ended || audio_dma.end_requested) {
        return 0u;
    }

    uint32_t queued = 0u;
    if (!audio_dma_ring_occupancy(&queued)) return 0u;
    uint32_t free_frames = PJS_AUDIO_DMA_RING_FRAMES - queued;
    if (frames > free_frames) frames = free_frames;

    uint32_t write_pos = audio_dma_write_pos;
    volatile uint32_t *ring = audio_dma_ring_uncached();
    for (uint32_t index = 0u; index < frames; ++index) {
        uint32_t source = index * 2u;
        uint32_t packed = (uint32_t)(uint16_t)interleaved[source] |
                          ((uint32_t)(uint16_t)interleaved[source + 1u] << 16);
        ring[(write_pos + index) & PJS_AUDIO_DMA_RING_MASK] = packed;
    }
    /* One writer owns write_pos; publish it only after every word is visible
     * through the same uncached alias used by DMA. */
    __asm__ volatile("" ::: "memory");
    audio_dma_write_pos = write_pos + frames;
    return frames;
}

int pjs_audio_dma_play(void)
{
    uint32_t saved = irq_save_disable();
    if (!audio_dma.initialized) {
        irq_restore(saved);
        return PJS_AUDIO_DMA_RESULT_NOT_READY;
    }
    if (audio_dma.fault != 0u) {
        irq_restore(saved);
        return PJS_AUDIO_DMA_RESULT_FAULT;
    }
    if (audio_dma.ended || audio_dma.drain_pending) {
        irq_restore(saved);
        return PJS_AUDIO_DMA_RESULT_BUSY;
    }

    audio_dma.pause_requested = false;
    audio_dma.paused = false;
    audio_dma.playing = true;
    PP_DMA_REQ_STATUS |= PP_DMA_REQ_IIS_MASK;
    PP_IISCONFIG |= PP_IIS_TXFIFOEN;
    if (!audio_dma.dma_active) {
        audio_dma.last_irq_us = timer_now_us();
        audio_dma_enable_irq();
        audio_dma_start_next();
    }
    int result = audio_dma.fault == 0u ? PJS_AUDIO_DMA_RESULT_OK :
                                        PJS_AUDIO_DMA_RESULT_FAULT;
    irq_restore(saved);
    return result;
}

int pjs_audio_dma_pause(void)
{
    uint32_t saved = irq_save_disable();
    if (!audio_dma.initialized) {
        irq_restore(saved);
        return PJS_AUDIO_DMA_RESULT_NOT_READY;
    }
    if (audio_dma.fault != 0u) {
        irq_restore(saved);
        return PJS_AUDIO_DMA_RESULT_FAULT;
    }
    if (audio_dma.ended || audio_dma.drain_pending) {
        irq_restore(saved);
        return PJS_AUDIO_DMA_RESULT_OK;
    }
    if (!audio_dma.playing) {
        audio_dma.paused = true;
        irq_restore(saved);
        return PJS_AUDIO_DMA_RESULT_OK;
    }

    /* Let the current command retire. The IRQ then stops at a DMA boundary,
     * leaving read_pos behind all queued source data for a stable pause. */
    audio_dma.pause_requested = true;
    if (!audio_dma.dma_active) {
        audio_dma.pause_requested = false;
        audio_dma.playing = false;
        audio_dma.paused = true;
    }
    irq_restore(saved);
    return PJS_AUDIO_DMA_RESULT_OK;
}

void pjs_audio_dma_end(void)
{
    uint32_t saved = irq_save_disable();
    if (!audio_dma.initialized || audio_dma.fault != 0u ||
        audio_dma.ended || audio_dma.end_requested) {
        irq_restore(saved);
        return;
    }
    audio_dma.end_requested = true;
    if (audio_dma.end_requests != UINT32_MAX) ++audio_dma.end_requests;
    if (audio_dma.playing && !audio_dma.dma_active) audio_dma_start_next();
    irq_restore(saved);
}

int pjs_audio_dma_stop(void)
{
    uint32_t saved = irq_save_disable();
    if (!audio_dma.initialized) {
        if (!audio_dma.hardware_probe_done || audio_dma.cleanup_pending) {
            audio_dma_disable_irq();
            int cleanup_result = audio_dma_quiesce_hardware();
            if (cleanup_result == PJS_AUDIO_DMA_RESULT_OK) {
                audio_dma.cleanup_pending = false;
                audio_dma.hardware_probe_done = true;
            } else {
                if (audio_dma.stop_timeouts != UINT32_MAX) {
                    ++audio_dma.stop_timeouts;
                }
                audio_dma_latch_fault(PJS_AUDIO_DMA_FAULT_STOP_TIMEOUT);
            }
            irq_restore(saved);
            return cleanup_result;
        }
        irq_restore(saved);
        return PJS_AUDIO_DMA_RESULT_OK;
    }

    int result = audio_dma_stop_hardware(true);
    if (result == PJS_AUDIO_DMA_RESULT_OK) {
        audio_dma_flush_ring();
        audio_dma.playing = false;
        audio_dma.paused = false;
        audio_dma.end_requested = false;
        audio_dma.in_flight_frames = 0u;
        audio_dma.in_flight_real = false;
        /* The FIFO and DMA request domain are now released. A repeated stop
         * (for example after codec close) must not touch those registers. */
        audio_dma.initialized = false;
        audio_dma.hardware_probe_done = true;
    }
    irq_restore(saved);
    return result;
}

void pjs_audio_dma_service(uint32_t now)
{
    uint32_t saved = irq_save_disable();
    /* The caller's tick timestamp may predate a DMA IRQ that ran just before
     * this critical section. Sample again after masking IRQs so unsigned
     * wrap-safe elapsed checks cannot see a spurious huge interval. */
    now = timer_now_us();
    if (!audio_dma.initialized || audio_dma.fault != 0u) {
        irq_restore(saved);
        return;
    }

    audio_dma_finish_end(now);
    if (audio_dma.fault == 0u && audio_dma.playing &&
        audio_dma.dma_active &&
        (uint32_t)(now - audio_dma.last_irq_us) >=
            PJS_AUDIO_DMA_WATCHDOG_US) {
        if (audio_dma.watchdog_faults != UINT32_MAX) {
            ++audio_dma.watchdog_faults;
        }
        audio_dma_latch_fault(PJS_AUDIO_DMA_FAULT_WATCHDOG);
        if (audio_dma_stop_hardware(true) == PJS_AUDIO_DMA_RESULT_OK) {
            audio_dma_flush_ring();
            audio_dma.playing = false;
            audio_dma.paused = false;
            audio_dma.initialized = false;
        }
    }
    irq_restore(saved);
}

void pjs_audio_dma_snapshot(PjsAudioDmaSnapshot *out)
{
    if (out == 0) return;
    uint32_t saved = irq_save_disable();
    uint32_t queued = 0u;
    (void)audio_dma_ring_occupancy(&queued);
    *out = (PjsAudioDmaSnapshot){
        .queued_frames = queued,
        .free_frames = PJS_AUDIO_DMA_RING_FRAMES - queued,
        .consumed_frames = audio_dma.consumed_frames,
        .underruns = audio_dma.underruns,
        .irq_count = audio_dma.irq_count,
        .playing = audio_dma.playing ? 1u : 0u,
        .paused = audio_dma.paused ? 1u : 0u,
        .ended = audio_dma.ended ? 1u : 0u,
        .fault = audio_dma.fault,
        .submitted_frames = audio_dma.submitted_frames,
        .silence_frames = audio_dma.silence_frames,
        .dma_restarts = audio_dma.dma_restarts,
        .watchdog_faults = audio_dma.watchdog_faults,
        .stop_timeouts = audio_dma.stop_timeouts,
        .drain_timeouts = audio_dma.drain_timeouts,
        .end_requests = audio_dma.end_requests,
        .end_events = audio_dma.end_events,
        .last_error = audio_dma.last_error,
        .cleanup_pending = audio_dma.cleanup_pending ? 1u : 0u,
    };
    irq_restore(saved);
}

int pjs_audio_dma_irq(void)
{
    /* Reading status acknowledges the DMA line. Do it before any branch so a
     * stale/foreign interrupt cannot retrigger forever. */
    uint32_t status = PP_DMA0_STATUS;
    if (!audio_dma.initialized) {
        audio_dma_disable_irq();
        PP_DMA0_CMD &= ~(PP_DMA_CMD_START | PP_DMA_CMD_INTR);
        return 1;
    }

    if ((status & PP_DMA_STATUS_INTR) == 0u ||
        (status & PP_DMA_STATUS_BUSY) != 0u || !audio_dma.dma_active ||
        audio_dma.in_flight_frames == 0u) {
        audio_dma_disable_irq();
        PP_DMA0_CMD &= ~(PP_DMA_CMD_START | PP_DMA_CMD_INTR);
        audio_dma_latch_fault(PJS_AUDIO_DMA_FAULT_STATUS);
        audio_dma.playing = false;
        audio_dma.paused = false;
        audio_dma.pause_requested = false;
        audio_dma.dma_active = false;
        return 1;
    }

    ++audio_dma.irq_count;
    audio_dma.last_irq_us = timer_now_us();
    uint32_t frames = audio_dma.in_flight_frames;
    bool real = audio_dma.in_flight_real;
    audio_dma.dma_active = false;
    audio_dma.in_flight_frames = 0u;
    audio_dma.in_flight_real = false;
    if (real) {
        audio_dma_read_pos += frames;
        audio_dma.consumed_frames += frames;
    } else {
        audio_dma.silence_frames += frames;
    }
    __asm__ volatile("" ::: "memory");

    if (audio_dma.pause_requested) {
        audio_dma.pause_requested = false;
        audio_dma.playing = false;
        audio_dma.paused = true;
        return 1;
    }
    if (audio_dma.playing) audio_dma_start_next();
    return 1;
}
