#include "audio_pcm_mixer.h"

#include "audio_dma.h"
#include "timer.h"

#include <limits.h>

#define SOURCE_MASK (PJS_AUDIO_PCM_RING_FRAMES - 1u)
#define LEDGER_COUNT 32u
#define LEDGER_MASK (LEDGER_COUNT - 1u)
#define MIX_STRIP_FRAMES 64u
#define HANDLE_SLOT_MASK 7u
#define HANDLE_GENERATION_MAX ((uint32_t)INT_MAX >> 3)

/* This is a separate portable mixer, not a mutation of the Campaign-4
 * diagnostic mixer. That keeps the hardware-qualified diagnostic path stable
 * while the globalThis.audio contract earns its own device gate. */
static int16_t pcm[PJS_AUDIO_PCM_MAX_STREAMS][PJS_AUDIO_PCM_RING_FRAMES][2];
static int16_t output[PJS_AUDIO_PCM_OUTPUT_CHUNK * 2u];
static int32_t accumulation[MIX_STRIP_FRAMES * 2u];
static uint32_t generations[PJS_AUDIO_PCM_MAX_STREAMS];

typedef struct {
    int handle;
    uint32_t rate;
    uint32_t channels;
    uint32_t ratio;
    uint32_t gain;
    uint32_t written;
    uint32_t mixed;
    uint32_t retired;
    uint32_t repeat;
    uint32_t underruns;
    uint32_t end_events;
    bool live;
    bool playing;
    bool end_requested;
    bool planned_starved;
    bool ended;
} Source;

typedef struct {
    uint32_t output_end;
    int handle[PJS_AUDIO_PCM_MAX_STREAMS];
    uint32_t source_end[PJS_AUDIO_PCM_MAX_STREAMS];
    uint32_t underruns[PJS_AUDIO_PCM_MAX_STREAMS];
} Ledger;

typedef struct {
    Source source[PJS_AUDIO_PCM_MAX_STREAMS];
    Ledger ledger[LEDGER_COUNT];
    uint32_t head;
    uint32_t pending;
    uint32_t output_written;
    bool initialized;
    uint8_t fault;
} Mixer;

static Mixer mixer;

static void latch_fault(uint8_t fault)
{
    if (mixer.fault == 0u) mixer.fault = fault;
}

static int handle_of(uint32_t slot, uint32_t generation)
{
    return (int)((generation << 3) | slot);
}

static Source *source_for_handle(int handle)
{
    if (!mixer.initialized || handle <= 0) return 0;
    uint32_t slot = (uint32_t)handle & HANDLE_SLOT_MASK;
    if (slot >= PJS_AUDIO_PCM_MAX_STREAMS) return 0;
    Source *source = &mixer.source[slot];
    return source->live && source->handle == handle ? source : 0;
}

static uint32_t next_generation(uint32_t slot)
{
    uint32_t generation = generations[slot] + 1u;
    if (generation == 0u || generation > HANDLE_GENERATION_MAX) generation = 1u;
    generations[slot] = generation;
    return generation;
}

static int16_t saturate(int32_t sample)
{
    if (sample > 32767) sample = 32767;
    if (sample < -32768) sample = -32768;
    return (int16_t)sample;
}

int pjs_audio_pcm_mixer_init(void)
{
    mixer = (Mixer){0};
    mixer.initialized = true;
    return 0;
}

int pjs_audio_pcm_mixer_create(uint32_t rate, uint32_t channels)
{
    if (!mixer.initialized || mixer.fault != 0u ||
        (rate != 44100u && rate != 22050u && rate != 11025u) ||
        (channels != 1u && channels != 2u)) return -1;

    for (uint32_t slot = 0u; slot < PJS_AUDIO_PCM_MAX_STREAMS; ++slot) {
        Source *source = &mixer.source[slot];
        if (source->live) continue;
        uint32_t generation = next_generation(slot);
        *source = (Source){
            .handle = handle_of(slot, generation),
            .rate = rate,
            .channels = channels,
            .ratio = rate == 44100u ? 1u : rate == 22050u ? 2u : 4u,
            .gain = PJS_AUDIO_PCM_GAIN_ONE,
            .live = true,
        };
        return source->handle;
    }
    return -1;
}

int pjs_audio_pcm_mixer_destroy(int handle)
{
    Source *source = source_for_handle(handle);
    if (source == 0) return -1;
    *source = (Source){0};
    return 0;
}

uint32_t pjs_audio_pcm_mixer_write(int handle, const int16_t *interleaved,
                                   uint32_t frames)
{
    Source *source = source_for_handle(handle);
    if (source == 0 || interleaved == 0 || mixer.fault != 0u ||
        source->end_requested) return 0u;

    uint32_t queued = source->written - source->retired;
    if (queued > PJS_AUDIO_PCM_RING_FRAMES) {
        latch_fault(1u);
        return 0u;
    }
    uint32_t available = PJS_AUDIO_PCM_RING_FRAMES - queued;
    if (frames > available) frames = available;
    uint32_t slot = (uint32_t)(source - mixer.source);
    for (uint32_t index = 0u; index < frames; ++index) {
        uint32_t at = (source->written + index) & SOURCE_MASK;
        int16_t left = interleaved[index * source->channels];
        pcm[slot][at][0] = left;
        pcm[slot][at][1] = source->channels == 1u ?
            left : interleaved[index * 2u + 1u];
    }
    source->written += frames;
    if (frames != 0u) source->ended = false;
    return frames;
}

int pjs_audio_pcm_mixer_play(int handle)
{
    Source *source = source_for_handle(handle);
    if (source == 0 || mixer.fault != 0u || source->end_requested) return -1;
    source->playing = true;
    source->ended = false;
    source->planned_starved = false;
    return 0;
}

int pjs_audio_pcm_mixer_pause(int handle)
{
    Source *source = source_for_handle(handle);
    if (source == 0 || mixer.fault != 0u) return -1;
    source->playing = false;
    source->planned_starved = false;
    return 0;
}

int pjs_audio_pcm_mixer_stop(int handle)
{
    Source *source = source_for_handle(handle);
    if (source == 0 || mixer.fault != 0u) return -1;
    source->playing = false;
    source->written = source->retired;
    source->mixed = source->retired;
    source->repeat = 0u;
    source->end_requested = false;
    source->planned_starved = false;
    source->ended = false;
    return 0;
}

int pjs_audio_pcm_mixer_volume(int handle, uint32_t q15)
{
    Source *source = source_for_handle(handle);
    if (source == 0 || mixer.fault != 0u || q15 > PJS_AUDIO_PCM_GAIN_ONE)
        return -1;
    source->gain = q15;
    return 0;
}

int pjs_audio_pcm_mixer_end(int handle)
{
    Source *source = source_for_handle(handle);
    if (source == 0 || mixer.fault != 0u) return -1;
    if (source->end_requested) return 0;
    source->end_requested = true;
    if (source->retired == source->written && source->mixed == source->written &&
        source->repeat == 0u) {
        source->playing = false;
        source->end_requested = false;
        source->ended = true;
        if (source->end_events != UINT32_MAX) ++source->end_events;
    }
    return 0;
}

void pjs_audio_pcm_mixer_service(uint32_t now)
{
    if (!mixer.initialized) return;
    pjs_audio_dma_service(now);
    PjsAudioDmaSnapshot dma;
    pjs_audio_dma_snapshot(&dma);
    if (dma.fault != 0u) {
        latch_fault(2u);
        return;
    }

    while (mixer.pending != 0u) {
        Ledger *entry = &mixer.ledger[mixer.head];
        if ((int32_t)(dma.consumed_frames - entry->output_end) < 0) break;
        for (uint32_t slot = 0u; slot < PJS_AUDIO_PCM_MAX_STREAMS; ++slot) {
            int handle = entry->handle[slot];
            if (handle <= 0) continue;
            Source *source = source_for_handle(handle);
            /* Destroy/reuse is safe even if a caller forgot to rebuild: old
             * ledgers can never retire frames into a new generation. */
            if (source == 0) continue;
            uint32_t end = entry->source_end[slot];
            if (end - source->retired > source->written - source->retired) {
                latch_fault(1u);
                return;
            }
            source->retired = end;
            source->underruns += entry->underruns[slot];
            if (source->end_requested && source->retired == source->written &&
                source->mixed == source->written && source->repeat == 0u) {
                source->playing = false;
                source->end_requested = false;
                source->ended = true;
                source->planned_starved = false;
                if (source->end_events != UINT32_MAX) ++source->end_events;
            }
        }
        mixer.head = (mixer.head + 1u) & LEDGER_MASK;
        --mixer.pending;
    }
}

bool pjs_audio_pcm_mixer_refill(void)
{
    if (!mixer.initialized || mixer.fault != 0u) return false;
    pjs_audio_pcm_mixer_service(timer_now_us());
    if (mixer.fault != 0u) return false;

    PjsAudioDmaSnapshot dma;
    pjs_audio_dma_snapshot(&dma);
    if (dma.ended || dma.queued_frames >= PJS_AUDIO_PCM_OUTPUT_HIGHWATER ||
        dma.free_frames < PJS_AUDIO_PCM_OUTPUT_CHUNK ||
        mixer.pending == LEDGER_COUNT) return false;

    bool any_playing = false;
    bool any_future = false;
    for (uint32_t slot = 0u; slot < PJS_AUDIO_PCM_MAX_STREAMS; ++slot) {
        Source *source = &mixer.source[slot];
        if (!source->live || !source->playing) continue;
        any_playing = true;
        if (!source->end_requested || source->mixed != source->written ||
            source->repeat != 0u) any_future = true;
    }
    if (!any_playing || !any_future) return false;

    uint32_t position[PJS_AUDIO_PCM_MAX_STREAMS];
    uint32_t repeat[PJS_AUDIO_PCM_MAX_STREAMS];
    bool starved[PJS_AUDIO_PCM_MAX_STREAMS];
    Ledger entry = {0};
    for (uint32_t slot = 0u; slot < PJS_AUDIO_PCM_MAX_STREAMS; ++slot) {
        position[slot] = mixer.source[slot].mixed;
        repeat[slot] = mixer.source[slot].repeat;
        starved[slot] = mixer.source[slot].planned_starved;
    }

    for (uint32_t base = 0u; base < PJS_AUDIO_PCM_OUTPUT_CHUNK;
         base += MIX_STRIP_FRAMES) {
        for (uint32_t sample = 0u; sample < MIX_STRIP_FRAMES * 2u; ++sample)
            accumulation[sample] = 0;

        for (uint32_t slot = 0u; slot < PJS_AUDIO_PCM_MAX_STREAMS; ++slot) {
            Source *source = &mixer.source[slot];
            if (!source->live || !source->playing) continue;
            /* Once an ended source's last frame has been planned, prior ledger
             * entries own its retirement. Do not manufacture tail silence for
             * it while another stream keeps the shared sink alive. */
            if (source->end_requested && position[slot] == source->written &&
                repeat[slot] == 0u) continue;

            entry.handle[slot] = source->handle;
            uint32_t at = position[slot];
            uint32_t phase = repeat[slot];
            uint32_t written = source->written;
            uint32_t ratio = source->ratio;
            uint32_t gain = source->gain;
            bool empty = starved[slot];
            uint32_t frame = 0u;
            while (frame < MIX_STRIP_FRAMES) {
                if (at == written) {
                    if (!source->end_requested && !empty) {
                        empty = true;
                        ++entry.underruns[slot];
                    }
                    break;
                }
                empty = false;
                uint32_t run = ratio - phase;
                if (run > MIX_STRIP_FRAMES - frame) run = MIX_STRIP_FRAMES - frame;
                if (gain != 0u) {
                    int32_t left = pcm[slot][at & SOURCE_MASK][0];
                    int32_t right = pcm[slot][at & SOURCE_MASK][1];
                    if (gain != PJS_AUDIO_PCM_GAIN_ONE) {
                        left = left * (int32_t)gain / 32768;
                        right = right * (int32_t)gain / 32768;
                    }
                    for (uint32_t n = 0u; n < run; ++n) {
                        accumulation[2u * (frame + n)] += left;
                        accumulation[2u * (frame + n) + 1u] += right;
                    }
                }
                frame += run;
                phase += run;
                if (phase == ratio) {
                    phase = 0u;
                    ++at;
                }
            }
            position[slot] = at;
            repeat[slot] = phase;
            starved[slot] = empty;
        }

        for (uint32_t sample = 0u; sample < MIX_STRIP_FRAMES * 2u; ++sample)
            output[2u * base + sample] = saturate(accumulation[sample]);
    }

    uint32_t accepted = pjs_audio_dma_write(output, PJS_AUDIO_PCM_OUTPUT_CHUNK);
    if (accepted != PJS_AUDIO_PCM_OUTPUT_CHUNK) {
        latch_fault(5u);
        return false;
    }
    mixer.output_written += accepted;
    entry.output_end = mixer.output_written;
    for (uint32_t slot = 0u; slot < PJS_AUDIO_PCM_MAX_STREAMS; ++slot) {
        entry.source_end[slot] = position[slot];
        Source *source = &mixer.source[slot];
        if (!source->live || !source->playing) continue;
        source->mixed = position[slot];
        source->repeat = repeat[slot];
        source->planned_starved = starved[slot];
    }
    mixer.ledger[(mixer.head + mixer.pending) & LEDGER_MASK] = entry;
    ++mixer.pending;
    return true;
}

void pjs_audio_pcm_mixer_discard_prepared(void)
{
    if (!mixer.initialized) return;
    mixer.head = 0u;
    mixer.pending = 0u;
    mixer.output_written = 0u;
    for (uint32_t slot = 0u; slot < PJS_AUDIO_PCM_MAX_STREAMS; ++slot) {
        Source *source = &mixer.source[slot];
        if (!source->live) continue;
        source->mixed = source->retired;
        source->repeat = 0u;
        source->planned_starved = false;
    }
}

int pjs_audio_pcm_mixer_snapshot(int handle, PjsAudioPcmMixerStream *out)
{
    if (out == 0) return -1;
    pjs_audio_pcm_mixer_service(timer_now_us());
    Source *source = source_for_handle(handle);
    if (source == 0) return -1;
    uint32_t queued = source->written - source->retired;
    if (queued > PJS_AUDIO_PCM_RING_FRAMES) {
        latch_fault(1u);
        return -1;
    }
    *out = (PjsAudioPcmMixerStream){
        .handle = source->handle,
        .rate = source->rate,
        .channels = (uint8_t)source->channels,
        .live = 1u,
        .playing = source->playing ? 1u : 0u,
        .ended = source->ended ? 1u : 0u,
        .end_requested = source->end_requested ? 1u : 0u,
        .queued_frames = queued,
        .free_frames = PJS_AUDIO_PCM_RING_FRAMES - queued,
        .unmixed_frames = source->written - source->mixed,
        .accepted_frames = source->written,
        .consumed_frames = source->retired,
        .underruns = source->underruns,
        .end_events = source->end_events,
    };
    return 0;
}

uint32_t pjs_audio_pcm_mixer_live_count(void)
{
    uint32_t count = 0u;
    for (uint32_t slot = 0u; slot < PJS_AUDIO_PCM_MAX_STREAMS; ++slot)
        if (mixer.source[slot].live) ++count;
    return count;
}

uint32_t pjs_audio_pcm_mixer_playing_count(void)
{
    uint32_t count = 0u;
    for (uint32_t slot = 0u; slot < PJS_AUDIO_PCM_MAX_STREAMS; ++slot)
        if (mixer.source[slot].live && mixer.source[slot].playing) ++count;
    return count;
}

bool pjs_audio_pcm_mixer_all_playing_draining(void)
{
    bool any = false;
    for (uint32_t slot = 0u; slot < PJS_AUDIO_PCM_MAX_STREAMS; ++slot) {
        Source *source = &mixer.source[slot];
        if (!source->live || !source->playing) continue;
        any = true;
        if (!source->end_requested || source->mixed != source->written ||
            source->repeat != 0u) return false;
    }
    return any;
}

void pjs_audio_pcm_mixer_aggregate(PjsAudioPcmMixerSnapshot *out)
{
    if (out == 0) return;
    pjs_audio_pcm_mixer_service(timer_now_us());
    PjsAudioDmaSnapshot dma;
    pjs_audio_dma_snapshot(&dma);
    *out = (PjsAudioPcmMixerSnapshot){
        .queued_output_frames = dma.queued_frames,
        .free_output_frames = dma.free_frames,
        .live_streams = pjs_audio_pcm_mixer_live_count(),
        .playing_streams = pjs_audio_pcm_mixer_playing_count(),
        .pending_ledgers = mixer.pending,
        .fault = mixer.fault,
    };
}
