#include "audio_pcm.h"

#include "audio.h"
#include "audio_clock.h"
#include "audio_dma.h"
#include "audio_pcm_mixer.h"
#include "timer.h"

#define PJS_AUDIO_PCM_CONTROL_TIMEOUT_US 100000u
#define PJS_AUDIO_PCM_UNMUTE_DELAY_US 10000u
#define PJS_AUDIO_PCM_EVENT_CAPACITY 32u
#define PJS_AUDIO_PCM_EVENT_MASK (PJS_AUDIO_PCM_EVENT_CAPACITY - 1u)
#define PJS_AUDIO_PCM_INGRESS_FRAMES 512u
#define PJS_AUDIO_PCM_SLOT_MASK 7u

enum { EVENT_NONE = 0u, EVENT_ENDED = 1u, EVENT_UNDERRUN = 2u, EVENT_CREDIT = 3u };

typedef struct {
    uint8_t type;
    int handle;
    uint32_t free_frames;
} Event;

typedef struct {
    int handle;
    uint32_t guest_free;
    uint32_t seen_underruns;
    uint32_t seen_end_events;
} EventState;

static struct {
    PjsAudioState codec;
    EventState slots[PJS_AUDIO_PCM_MAX_STREAMS];
    Event events[PJS_AUDIO_PCM_EVENT_CAPACITY];
    uint32_t event_head;
    uint32_t event_count;
    uint32_t last_error;
    bool engine;
    bool suspended;
    bool quiesce_failed;
    bool transport_playing;
    bool drain_requested;
    bool muted;
} pcm_state;

static int16_t ingress[PJS_AUDIO_PCM_INGRESS_FRAMES * 2u];
static char poll_text[PJS_AUDIO_PCM_POLL_BYTES];

static uint32_t slot_of_handle(int handle)
{
    return (uint32_t)handle & PJS_AUDIO_PCM_SLOT_MASK;
}

static bool valid_format(uint32_t rate, uint32_t channels)
{
    return (rate == 44100u || rate == 22050u || rate == 11025u) &&
           (channels == 1u || channels == 2u);
}

static void remember_error(uint32_t error)
{
    if (error != 0u) pcm_state.last_error = error;
}

static void clear_events(void)
{
    pcm_state.event_head = 0u;
    pcm_state.event_count = 0u;
    for (uint32_t slot = 0u; slot < PJS_AUDIO_PCM_MAX_STREAMS; ++slot)
        pcm_state.slots[slot] = (EventState){0};
}

static void purge_handle_events(int handle)
{
    Event kept[PJS_AUDIO_PCM_EVENT_CAPACITY];
    uint32_t count = 0u;
    for (uint32_t index = 0u; index < pcm_state.event_count; ++index) {
        Event event = pcm_state.events[(pcm_state.event_head + index) &
                                       PJS_AUDIO_PCM_EVENT_MASK];
        if (event.handle != handle) kept[count++] = event;
    }
    pcm_state.event_head = 0u;
    pcm_state.event_count = count;
    for (uint32_t index = 0u; index < count; ++index) pcm_state.events[index] = kept[index];
}

static void queue_event(uint8_t type, int handle, uint32_t free_frames)
{
    if (pcm_state.event_count == PJS_AUDIO_PCM_EVENT_CAPACITY) {
        /* The framework drains poll() every app turn. Overflow means the host
         * contract is no longer being observed; retain the oldest facts and
         * expose a diagnostic error instead of silently reordering them. */
        remember_error(0x45564e54u); /* EVNT */
        return;
    }
    uint32_t tail = (pcm_state.event_head + pcm_state.event_count) &
                    PJS_AUDIO_PCM_EVENT_MASK;
    pcm_state.events[tail] = (Event){
        .type = type, .handle = handle, .free_frames = free_frames,
    };
    ++pcm_state.event_count;
}

static int wait_pause_boundary(void)
{
    PjsAudioDmaSnapshot dma;
    pjs_audio_dma_snapshot(&dma);
    if (!dma.playing) return 0;
    int result = pjs_audio_dma_pause();
    if (result != PJS_AUDIO_DMA_RESULT_OK) return -1;
    uint32_t deadline = timer_now_us() + PJS_AUDIO_PCM_CONTROL_TIMEOUT_US;
    for (;;) {
        pjs_audio_dma_service(timer_now_us());
        pjs_audio_dma_snapshot(&dma);
        if (dma.fault != 0u) return -1;
        if (dma.paused || !dma.playing) return 0;
        if ((int32_t)(timer_now_us() - deadline) >= 0) return -1;
    }
}

static bool refill_to_reserve(void)
{
    bool wrote = false;
    for (uint32_t block = 0u;
         block < PJS_AUDIO_PCM_OUTPUT_HIGHWATER / PJS_AUDIO_PCM_OUTPUT_CHUNK;
         ++block) {
        PjsAudioDmaSnapshot dma;
        pjs_audio_dma_snapshot(&dma);
        if (dma.queued_frames >= PJS_AUDIO_PCM_OUTPUT_HIGHWATER) break;
        if (!pjs_audio_pcm_mixer_refill()) break;
        wrote = true;
    }
    return wrote;
}

static int resume_transport_if_needed(void)
{
    if (pjs_audio_pcm_mixer_playing_count() == 0u) return 0;
    (void)refill_to_reserve();
    PjsAudioDmaSnapshot dma;
    pjs_audio_dma_snapshot(&dma);
    if (dma.fault != 0u) return -1;
    if (dma.queued_frames == 0u) return 0;
    if (!dma.playing) {
        if (pjs_audio_dma_play() != PJS_AUDIO_DMA_RESULT_OK) return -1;
        pcm_state.transport_playing = true;
        if (pcm_state.muted) {
            timer_delay_us(PJS_AUDIO_PCM_UNMUTE_DELAY_US);
            if (pjs_audio_pcm_mute(&pcm_state.codec, false) != PJS_AUDIO_RESULT_OK)
                return -1;
            pcm_state.muted = false;
        }
    }
    return 0;
}

static int rebuild_prepared_output(void)
{
    if (!pcm_state.engine) return 0;
    if (wait_pause_boundary() != 0) {
        remember_error(0x50415553u); /* PAUS */
        return -1;
    }
    pjs_audio_pcm_mixer_service(timer_now_us());
    if (!pcm_state.muted) {
        if (pjs_audio_pcm_mute(&pcm_state.codec, true) != PJS_AUDIO_RESULT_OK) {
            remember_error(0x4d555445u); /* MUTE */
            return -1;
        }
        pcm_state.muted = true;
    }
    if (pjs_audio_dma_stop() != PJS_AUDIO_DMA_RESULT_OK ||
        pjs_audio_dma_init() != PJS_AUDIO_DMA_RESULT_OK) {
        remember_error(0x444d4146u); /* DMAF */
        return -1;
    }
    pcm_state.transport_playing = false;
    pcm_state.drain_requested = false;
    pjs_audio_pcm_mixer_discard_prepared();
    if (resume_transport_if_needed() != 0) {
        remember_error(0x52534d45u); /* RSME */
        return -1;
    }
    return 0;
}

static int engine_start(void)
{
    if (pcm_state.engine) return 0;
    /* The Campaign-4 gate first proved inherited DMA ownership is quiescent,
     * then took the PP5020 clock, then opened the muted codec. Preserve that
     * ordering exactly; the portable layer adds no hardware sequence. */
    if (pjs_audio_dma_stop() != PJS_AUDIO_DMA_RESULT_OK) {
        remember_error(0x444d4151u); /* DMAQ */
        return -1;
    }
    if (pjs_audio_clock_acquire() != 0) {
        remember_error(0x434c4b41u); /* CLKA */
        return -1;
    }
    pjs_audio_state_init(&pcm_state.codec);
    if (pjs_audio_pcm_prepare(&pcm_state.codec) != PJS_AUDIO_RESULT_OK) {
        remember_error(0x434f4443u); /* CODC */
        (void)pjs_audio_clock_release();
        return -1;
    }
    if (pjs_audio_dma_init() != PJS_AUDIO_DMA_RESULT_OK ||
        pjs_audio_pcm_mixer_init() != 0) {
        remember_error(0x4d495845u); /* MIXE */
        (void)pjs_audio_dma_stop();
        (void)pjs_audio_stop(&pcm_state.codec);
        (void)pjs_audio_clock_release();
        return -1;
    }
    pcm_state.engine = true;
    pcm_state.suspended = false;
    pcm_state.quiesce_failed = false;
    pcm_state.transport_playing = false;
    pcm_state.drain_requested = false;
    pcm_state.muted = true;
    pcm_state.last_error = 0u;
    clear_events();
    return 0;
}

static int engine_stop(void)
{
    if (!pcm_state.engine) {
        clear_events();
        return 0;
    }
    if (pcm_state.suspended && !pcm_state.quiesce_failed) {
        (void)pjs_audio_pcm_mixer_init();
        pcm_state.engine = false;
        pcm_state.suspended = false;
        pcm_state.quiesce_failed = false;
        pcm_state.transport_playing = false;
        pcm_state.drain_requested = false;
        clear_events();
        return 0;
    }
    int result = 0;
    if (!pcm_state.muted &&
        pjs_audio_pcm_mute(&pcm_state.codec, true) != PJS_AUDIO_RESULT_OK) {
        remember_error(0x4d555445u);
        result = -1;
    } else {
        pcm_state.muted = true;
    }
    if (result == 0) timer_delay_us(PJS_AUDIO_PCM_UNMUTE_DELAY_US);
    if (pjs_audio_dma_stop() != PJS_AUDIO_DMA_RESULT_OK) {
        remember_error(0x444d4153u); /* DMAS */
        result = -1;
    }
    if (result == 0 && pjs_audio_stop(&pcm_state.codec) != PJS_AUDIO_RESULT_OK) {
        remember_error(0x434f4453u); /* CODS */
        result = -1;
    }
    /* As in the qualified diagnostic gate, never restore the inherited clock
     * until DMA and codec teardown both completed. */
    if (result == 0 && pjs_audio_clock_release() != 0) {
        remember_error(0x434c4b52u); /* CLKR */
        result = -1;
    }
    if (result == 0) {
        (void)pjs_audio_pcm_mixer_init();
        pcm_state.engine = false;
        pcm_state.suspended = false;
        pcm_state.quiesce_failed = false;
        pcm_state.transport_playing = false;
        pcm_state.drain_requested = false;
        pcm_state.muted = true;
        clear_events();
    }
    return result;
}

int pjs_audio_pcm_create_stream(uint32_t sample_rate, uint32_t channels)
{
    if (!valid_format(sample_rate, channels)) return -1;
    if (engine_start() != 0) return -1;
    int handle = pjs_audio_pcm_mixer_create(sample_rate, channels);
    if (handle < 1) {
        if (pjs_audio_pcm_mixer_live_count() == 0u) (void)engine_stop();
        return -1;
    }
    uint32_t slot = slot_of_handle(handle);
    pcm_state.slots[slot] = (EventState){
        .handle = handle,
        .guest_free = PJS_AUDIO_PCM_RING_FRAMES,
    };
    return handle;
}

void pjs_audio_pcm_destroy_stream(int handle)
{
    if (!pcm_state.engine) return;
    PjsAudioPcmMixerStream before;
    if (pjs_audio_pcm_mixer_snapshot(handle, &before) != 0) return;
    (void)pjs_audio_pcm_mixer_destroy(handle);
    purge_handle_events(handle);
    uint32_t slot = slot_of_handle(handle);
    if (slot < PJS_AUDIO_PCM_MAX_STREAMS && pcm_state.slots[slot].handle == handle)
        pcm_state.slots[slot] = (EventState){0};
    if (pjs_audio_pcm_mixer_live_count() == 0u) {
        (void)engine_stop();
        return;
    }
    (void)rebuild_prepared_output();
}

uint32_t pjs_audio_pcm_write(int handle, const int16_t *pcm, uint32_t frames)
{
    if (!pcm_state.engine || pcm == 0 || frames == 0u) return 0u;
    uint32_t accepted = pjs_audio_pcm_mixer_write(handle, pcm, frames);
    uint32_t slot = slot_of_handle(handle);
    if (slot < PJS_AUDIO_PCM_MAX_STREAMS && pcm_state.slots[slot].handle == handle) {
        EventState *event = &pcm_state.slots[slot];
        event->guest_free = accepted > event->guest_free ? 0u :
                            event->guest_free - accepted;
    }
    return accepted;
}

uint32_t pjs_audio_pcm_write_bytes(int handle, const uint8_t *bytes, size_t length)
{
    if (!pcm_state.engine || bytes == 0 || length == 0u) return 0u;
    PjsAudioPcmMixerStream stream;
    if (pjs_audio_pcm_mixer_snapshot(handle, &stream) != 0) return 0u;
    size_t frame_bytes = (size_t)stream.channels * 2u;
    if (frame_bytes == 0u || (length % frame_bytes) != 0u) return 0u;
    uint32_t frames = (uint32_t)(length / frame_bytes);
    uint32_t total = 0u;
    while (total < frames) {
        uint32_t count = frames - total;
        if (count > PJS_AUDIO_PCM_INGRESS_FRAMES) count = PJS_AUDIO_PCM_INGRESS_FRAMES;
        for (uint32_t frame = 0u; frame < count; ++frame) {
            for (uint32_t channel = 0u; channel < stream.channels; ++channel) {
                size_t at = ((size_t)total + frame) * frame_bytes + channel * 2u;
                ingress[frame * stream.channels + channel] =
                    (int16_t)((uint16_t)bytes[at] | ((uint16_t)bytes[at + 1u] << 8));
            }
        }
        uint32_t accepted = pjs_audio_pcm_write(handle, ingress, count);
        total += accepted;
        if (accepted != count) break;
    }
    return total;
}

void pjs_audio_pcm_play(int handle)
{
    if (!pcm_state.engine || pjs_audio_pcm_mixer_play(handle) != 0) return;
    if (pcm_state.suspended) return;
    (void)rebuild_prepared_output();
}

void pjs_audio_pcm_pause(int handle)
{
    if (!pcm_state.engine || pjs_audio_pcm_mixer_pause(handle) != 0) return;
    if (pcm_state.suspended) return;
    (void)rebuild_prepared_output();
}

void pjs_audio_pcm_stop(int handle)
{
    if (!pcm_state.engine) return;
    PjsAudioPcmMixerStream stream;
    if (pjs_audio_pcm_mixer_snapshot(handle, &stream) != 0) return;

    /* stop() changes the source write cursor. Retire the currently executing
     * shared DMA block before truncating that cursor, otherwise a ledger for
     * the block that just reached the hardware clock could appear to retire
     * past the newly-flushed writer position. pause()/volume() do not move the
     * writer and therefore may use the ordinary rebuild helper. */
    if (wait_pause_boundary() != 0) {
        remember_error(0x50415553u); /* PAUS */
        return;
    }
    pjs_audio_pcm_mixer_service(timer_now_us());
    if (!pcm_state.muted) {
        if (pjs_audio_pcm_mute(&pcm_state.codec, true) != PJS_AUDIO_RESULT_OK) {
            remember_error(0x4d555445u); /* MUTE */
            return;
        }
        pcm_state.muted = true;
    }
    if (pjs_audio_dma_stop() != PJS_AUDIO_DMA_RESULT_OK ||
        pjs_audio_dma_init() != PJS_AUDIO_DMA_RESULT_OK) {
        remember_error(0x444d4146u); /* DMAF */
        return;
    }
    pcm_state.transport_playing = false;
    pcm_state.drain_requested = false;
    pjs_audio_pcm_mixer_discard_prepared();
    if (pjs_audio_pcm_mixer_stop(handle) != 0) return;

    uint32_t slot = slot_of_handle(handle);
    if (slot < PJS_AUDIO_PCM_MAX_STREAMS &&
        pcm_state.slots[slot].handle == handle) {
        pcm_state.slots[slot].guest_free = PJS_AUDIO_PCM_RING_FRAMES;
    }
    if (resume_transport_if_needed() != 0) remember_error(0x52534d45u);
}

void pjs_audio_pcm_set_volume(int handle, double volume)
{
    if (!pcm_state.engine) return;
    if (volume < 0.0) volume = 0.0;
    if (volume > 1.0) volume = 1.0;
    uint32_t gain = (uint32_t)(volume * (double)PJS_AUDIO_PCM_GAIN_ONE + 0.5);
    PjsAudioPcmMixerStream stream;
    if (pjs_audio_pcm_mixer_snapshot(handle, &stream) != 0 ||
        pjs_audio_pcm_mixer_volume(handle, gain) != 0) return;
    if (stream.playing) (void)rebuild_prepared_output();
}

void pjs_audio_pcm_end_stream(int handle)
{
    if (!pcm_state.engine) return;
    (void)pjs_audio_pcm_mixer_end(handle);
    pjs_audio_pcm_service();
}

void pjs_audio_pcm_service(void)
{
    if (!pcm_state.engine || pcm_state.suspended || pcm_state.quiesce_failed) return;
    pjs_audio_pcm_mixer_service(timer_now_us());
    PjsAudioPcmMixerSnapshot mixed;
    pjs_audio_pcm_mixer_aggregate(&mixed);
    if (mixed.fault != 0u) {
        remember_error(0x4d495846u); /* MIXF */
        return;
    }

    PjsAudioDmaSnapshot dma;
    pjs_audio_dma_snapshot(&dma);
    if (dma.fault != 0u) {
        remember_error(0x444d4146u);
        return;
    }

    if (!pcm_state.drain_requested &&
        pjs_audio_pcm_mixer_all_playing_draining()) {
        /* No more source samples can be planned. Let the already-prepared
         * shared output retire, then use the qualified FIFO-drain end path. */
        pjs_audio_dma_end();
        pcm_state.drain_requested = true;
    } else if (!pcm_state.drain_requested && mixed.playing_streams != 0u) {
        (void)pjs_audio_pcm_mixer_refill();
        if (resume_transport_if_needed() != 0) {
            remember_error(0x52534d45u);
            return;
        }
    }

    pjs_audio_dma_service(timer_now_us());
    pjs_audio_pcm_mixer_service(timer_now_us());
    pjs_audio_dma_snapshot(&dma);
    if (pcm_state.drain_requested && dma.ended) {
        /* Individual ended edges were retired from the final ledger above.
         * Re-arm an empty transport so a live handle may be written and
         * replayed after its ended event without reopening the codec/clock. */
        if (pjs_audio_dma_stop() != PJS_AUDIO_DMA_RESULT_OK ||
            pjs_audio_dma_init() != PJS_AUDIO_DMA_RESULT_OK) {
            remember_error(0x444d4152u); /* DMAR */
            return;
        }
        pjs_audio_pcm_mixer_discard_prepared();
        pcm_state.transport_playing = false;
        pcm_state.drain_requested = false;
        if (!pcm_state.muted) {
            (void)pjs_audio_pcm_mute(&pcm_state.codec, true);
            pcm_state.muted = true;
        }
    }
}

bool pjs_audio_pcm_needs_service(void)
{
    if (!pcm_state.engine || pcm_state.suspended || pcm_state.quiesce_failed || pcm_state.drain_requested ||
        pjs_audio_pcm_mixer_playing_count() == 0u) return false;
    PjsAudioDmaSnapshot dma;
    pjs_audio_dma_snapshot(&dma);
    return dma.fault == 0u &&
           (!dma.playing || dma.queued_frames < PJS_AUDIO_PCM_OUTPUT_LOWWATER);
}

void pjs_audio_pcm_begin_tick(void)
{
    if (!pcm_state.engine) return;
    pjs_audio_pcm_service();
    for (uint32_t slot = 0u; slot < PJS_AUDIO_PCM_MAX_STREAMS; ++slot) {
        EventState *event = &pcm_state.slots[slot];
        if (event->handle <= 0) continue;
        PjsAudioPcmMixerStream stream;
        if (pjs_audio_pcm_mixer_snapshot(event->handle, &stream) != 0) continue;
        if (stream.end_events != event->seen_end_events) {
            event->seen_end_events = stream.end_events;
            queue_event(EVENT_ENDED, event->handle, 0u);
        }
        if (stream.underruns != event->seen_underruns) {
            event->seen_underruns = stream.underruns;
            queue_event(EVENT_UNDERRUN, event->handle, 0u);
        }
        if (stream.free_frames != event->guest_free) {
            event->guest_free = stream.free_frames;
            queue_event(EVENT_CREDIT, event->handle, stream.free_frames);
        }
    }
}

static char *append_text(char *out, const char *end, const char *text)
{
    while (*text != '\0' && out < end) *out++ = *text++;
    return out;
}

static char *append_u32(char *out, const char *end, uint32_t value)
{
    char digits[10];
    uint32_t count = 0u;
    do {
        digits[count++] = (char)('0' + value % 10u);
        value /= 10u;
    } while (value != 0u && count < sizeof(digits));
    while (count != 0u && out < end) *out++ = digits[--count];
    return out;
}

const char *pjs_audio_pcm_poll(void)
{
    if (pcm_state.event_count == 0u) return 0;
    Event event = pcm_state.events[pcm_state.event_head];
    pcm_state.event_head = (pcm_state.event_head + 1u) & PJS_AUDIO_PCM_EVENT_MASK;
    --pcm_state.event_count;

    char *out = poll_text;
    char *end = poll_text + sizeof(poll_text) - 1u;
    out = append_text(out, end, "{\"t\":\"");
    out = append_text(out, end, event.type == EVENT_ENDED ? "ended" :
        event.type == EVENT_UNDERRUN ? "underrun" : "credit");
    out = append_text(out, end, "\",\"h\":");
    out = append_u32(out, end, (uint32_t)event.handle);
    if (event.type == EVENT_CREDIT) {
        out = append_text(out, end, ",\"free\":");
        out = append_u32(out, end, event.free_frames);
    }
    if (out < end) *out++ = '}';
    *out = '\0';
    return poll_text;
}

bool pjs_audio_pcm_active(void)
{
    return pcm_state.engine;
}

bool pjs_audio_pcm_busy(void)
{
    if (!pcm_state.engine) return false;
    if (pcm_state.quiesce_failed) return true;
    return pjs_audio_pcm_mixer_playing_count() != 0u ||
           pjs_audio_pcm_mixer_all_playing_draining() ||
           pcm_state.transport_playing || pcm_state.drain_requested;
}

int pjs_audio_pcm_suspend(void)
{
    if (pcm_state.quiesce_failed) return PJS_AUDIO_PCM_ERR_FAULT;
    if (!pcm_state.engine || pcm_state.suspended) return PJS_AUDIO_PCM_OK;
    /* A playing or draining source must remain clocked. Refusing here keeps
     * the source ring and event ledger intact until the guest pauses/stops. */
    if (pjs_audio_pcm_busy()) return PJS_AUDIO_PCM_ERR_CONTROL;

    if (!pcm_state.muted &&
        pjs_audio_pcm_mute(&pcm_state.codec, true) != PJS_AUDIO_RESULT_OK) {
        remember_error(0x4d555445u);
        pcm_state.quiesce_failed = true;
        return PJS_AUDIO_PCM_ERR_CONTROL;
    }
    pcm_state.muted = true;
    if (pjs_audio_dma_stop() != PJS_AUDIO_DMA_RESULT_OK) {
        remember_error(0x444d4153u);
        pcm_state.quiesce_failed = true;
        return PJS_AUDIO_PCM_ERR_FAULT;
    }
    /* The DMA API flushes its hardware ring. Only now is it safe to discard
     * the mixer ledger for a speculative block; source PCM remains queued. */
    pjs_audio_pcm_mixer_discard_prepared();
    if (pjs_audio_stop(&pcm_state.codec) != PJS_AUDIO_RESULT_OK) {
        remember_error(0x434f4453u);
        pcm_state.quiesce_failed = true;
        return PJS_AUDIO_PCM_ERR_CONTROL;
    }
    if (pjs_audio_clock_release() != 0) {
        remember_error(0x434c4b52u);
        pcm_state.quiesce_failed = true;
        return PJS_AUDIO_PCM_ERR_CONTROL;
    }
    pcm_state.transport_playing = false;
    pcm_state.drain_requested = false;
    pcm_state.suspended = true;
    pcm_state.quiesce_failed = false;
    return PJS_AUDIO_PCM_OK;
}

int pjs_audio_pcm_resume(void)
{
    if (pcm_state.quiesce_failed) return PJS_AUDIO_PCM_ERR_FAULT;
    if (!pcm_state.engine || !pcm_state.suspended) return PJS_AUDIO_PCM_OK;
    bool clock_acquired = pjs_audio_clock_acquire() == 0;
    if (!clock_acquired ||
        pjs_audio_pcm_prepare(&pcm_state.codec) != PJS_AUDIO_RESULT_OK ||
        pjs_audio_dma_init() != PJS_AUDIO_DMA_RESULT_OK) {
        remember_error(0x52534d45u);
        /* Fail closed: leave the facade suspended and ensure no service path
         * touches partially restored hardware. */
        if (pjs_audio_dma_stop() == PJS_AUDIO_DMA_RESULT_OK &&
            pjs_audio_stop(&pcm_state.codec) == PJS_AUDIO_RESULT_OK) {
            (void)pjs_audio_clock_release();
        }
        pcm_state.suspended = true;
        pcm_state.quiesce_failed = true;
        pcm_state.transport_playing = false;
        return PJS_AUDIO_PCM_ERR_FAULT;
    }
    pcm_state.suspended = false;
    pcm_state.muted = true;
    pjs_audio_pcm_mixer_discard_prepared();
    if (resume_transport_if_needed() != 0) {
        remember_error(0x52534d45u);
        if (pjs_audio_dma_stop() == PJS_AUDIO_DMA_RESULT_OK &&
            pjs_audio_stop(&pcm_state.codec) == PJS_AUDIO_RESULT_OK) {
            (void)pjs_audio_clock_release();
        }
        pcm_state.suspended = true;
        pcm_state.quiesce_failed = true;
        pcm_state.transport_playing = false;
        return PJS_AUDIO_PCM_ERR_FAULT;
    }
    return PJS_AUDIO_PCM_OK;
}

int pjs_audio_pcm_reset(void)
{
    return engine_stop();
}

uint32_t pjs_audio_pcm_last_error(void)
{
    return pcm_state.last_error;
}

void pjs_audio_stream_gate_refill(void)
{
    pjs_audio_pcm_service();
}

/* Link-compatible aliases for main/Rust cooperative sites that were qualified
 * before the portable namespace existed. AUDIO_PCM_GATE never links the
 * diagnostic audio_stream_gate.c, so these symbols cannot collide. */
int pjs_audio_stream_gate_start(PjsAudioState *audio)
{
    (void)audio;
    return -1;
}

void pjs_audio_stream_gate_tick(uint32_t now)
{
    (void)now;
    pjs_audio_pcm_service();
}

bool pjs_audio_stream_gate_needs_service(void)
{
    return pjs_audio_pcm_needs_service();
}

int pjs_audio_stream_gate_cancel(void)
{
    return pjs_audio_pcm_reset();
}

bool pjs_audio_stream_gate_active(void)
{
    return pjs_audio_pcm_active();
}

bool pjs_audio_stream_gate_status(uint32_t *mode, uint32_t *error)
{
    if (mode != 0) *mode = 0u;
    if (error != 0) *error = pjs_audio_pcm_last_error();
    return false;
}
