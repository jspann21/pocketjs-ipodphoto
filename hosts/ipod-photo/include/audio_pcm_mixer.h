#ifndef POCKETJS_IPOD_PHOTO_AUDIO_PCM_MIXER_H
#define POCKETJS_IPOD_PHOTO_AUDIO_PCM_MIXER_H

#include <stdbool.h>
#include <stdint.h>

#define PJS_AUDIO_PCM_MAX_STREAMS 4u
#define PJS_AUDIO_PCM_RING_FRAMES 16384u
#define PJS_AUDIO_PCM_OUTPUT_RATE 44100u
#define PJS_AUDIO_PCM_OUTPUT_CHUNK 1024u
/* Portable controls flush/rebuild the shared prepared queue. Keep the queue
 * deliberately shorter than the Campaign-4 diagnostic reserve so a newly
 * played/stopped stream reaches the hardware clock promptly. */
#define PJS_AUDIO_PCM_OUTPUT_HIGHWATER 4096u
#define PJS_AUDIO_PCM_OUTPUT_LOWWATER 2048u
#define PJS_AUDIO_PCM_GAIN_ONE 32768u

typedef struct {
    int handle;
    uint32_t rate;
    uint8_t channels;
    uint8_t live;
    uint8_t playing;
    uint8_t ended;
    uint8_t end_requested;
    uint32_t queued_frames;
    uint32_t free_frames;
    uint32_t unmixed_frames;
    uint32_t accepted_frames;
    uint32_t consumed_frames;
    uint32_t underruns;
    uint32_t end_events;
} PjsAudioPcmMixerStream;

typedef struct {
    uint32_t queued_output_frames;
    uint32_t free_output_frames;
    uint32_t live_streams;
    uint32_t playing_streams;
    uint32_t pending_ledgers;
    uint8_t fault;
} PjsAudioPcmMixerSnapshot;

int pjs_audio_pcm_mixer_init(void);
int pjs_audio_pcm_mixer_create(uint32_t rate, uint32_t channels);
int pjs_audio_pcm_mixer_destroy(int handle);
uint32_t pjs_audio_pcm_mixer_write(int handle, const int16_t *interleaved,
                                   uint32_t frames);
int pjs_audio_pcm_mixer_play(int handle);
int pjs_audio_pcm_mixer_pause(int handle);
int pjs_audio_pcm_mixer_stop(int handle);
int pjs_audio_pcm_mixer_volume(int handle, uint32_t q15);
int pjs_audio_pcm_mixer_end(int handle);
void pjs_audio_pcm_mixer_service(uint32_t now);
/* Prepare at most one 1024-frame output block. */
bool pjs_audio_pcm_mixer_refill(void);
/* Called only after the facade has stopped/reinitialized the shared DMA ring.
 * No source frame is retired merely because it had been speculatively mixed. */
void pjs_audio_pcm_mixer_discard_prepared(void);
int pjs_audio_pcm_mixer_snapshot(int handle, PjsAudioPcmMixerStream *out);
void pjs_audio_pcm_mixer_aggregate(PjsAudioPcmMixerSnapshot *out);
uint32_t pjs_audio_pcm_mixer_live_count(void);
uint32_t pjs_audio_pcm_mixer_playing_count(void);
/* True when every currently playing source has no future source frame to mix
 * because endStream was requested and all written input is already planned. */
bool pjs_audio_pcm_mixer_all_playing_draining(void);

#endif
