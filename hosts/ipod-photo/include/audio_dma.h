#ifndef POCKETJS_IPOD_PHOTO_AUDIO_DMA_H
#define POCKETJS_IPOD_PHOTO_AUDIO_DMA_H

#include <stdint.h>

/* The first native streaming gate deliberately owns one fixed-format sink.
 * The portable audio contract is not exposed until rate conversion, mixing,
 * and guest ownership have passed their own gates. */
#define PJS_AUDIO_DMA_RATE          44100u
#define PJS_AUDIO_DMA_CHANNELS     2u
#define PJS_AUDIO_DMA_RING_FRAMES  16384u
#define PJS_AUDIO_DMA_CHUNK_FRAMES 1024u

enum {
    PJS_AUDIO_DMA_RESULT_OK = 0,
    PJS_AUDIO_DMA_RESULT_ARGUMENT = -1,
    PJS_AUDIO_DMA_RESULT_BUSY = -2,
    PJS_AUDIO_DMA_RESULT_NOT_READY = -3,
    PJS_AUDIO_DMA_RESULT_FAULT = -4,
    PJS_AUDIO_DMA_RESULT_TIMEOUT = -5,
};

/* Fault values are intentionally positive because zero means no fault in the
 * sparse main-loop snapshot. Once set, the stream is fail-closed until init.
 */
enum {
    PJS_AUDIO_DMA_FAULT_NONE = 0u,
    PJS_AUDIO_DMA_FAULT_STOP_TIMEOUT = 1u,
    PJS_AUDIO_DMA_FAULT_WATCHDOG = 2u,
    PJS_AUDIO_DMA_FAULT_RING = 3u,
    PJS_AUDIO_DMA_FAULT_STATUS = 4u,
    PJS_AUDIO_DMA_FAULT_DRAIN_TIMEOUT = 5u,
};

typedef struct {
    /* Required by the native stream gate. queued_frames includes the DMA
     * in-flight source block; consumed_frames excludes silence fallback. */
    uint32_t queued_frames;
    uint32_t free_frames;
    uint32_t consumed_frames;
    uint32_t underruns;
    uint32_t irq_count;
    uint8_t playing;
    uint8_t paused;
    uint8_t ended;
    uint8_t fault;

    /* Additional sparse counters useful when a hardware run fails. */
    uint32_t submitted_frames;
    uint32_t silence_frames;
    uint32_t dma_restarts;
    uint32_t watchdog_faults;
    uint32_t stop_timeouts;
    uint32_t drain_timeouts;
    uint32_t end_requests;
    uint32_t end_events;
    uint32_t last_error;
    uint8_t cleanup_pending;
} PjsAudioDmaSnapshot;

int pjs_audio_dma_init(void);
uint32_t pjs_audio_dma_write(const int16_t *interleaved, uint32_t frames);
int pjs_audio_dma_play(void);
int pjs_audio_dma_pause(void);
void pjs_audio_dma_end(void);
int pjs_audio_dma_stop(void);
void pjs_audio_dma_service(uint32_t now);
void pjs_audio_dma_snapshot(PjsAudioDmaSnapshot *out);

/* Normal IRQ dispatch only. This is intentionally not part of the guest or
 * stream-gate API; irq.c invokes it to acknowledge DMA0 completion. */
int pjs_audio_dma_irq(void);

#endif
