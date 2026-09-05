#ifndef POCKETJS_IPOD_PHOTO_AUDIO_H
#define POCKETJS_IPOD_PHOTO_AUDIO_H

#include <stdint.h>
#include <stdbool.h>

enum {
    PJS_AUDIO_OFF = 0u,
    PJS_AUDIO_READY = 1u,
    PJS_AUDIO_TONE = 2u,
    PJS_AUDIO_FAULT = 3u,
};

enum {
    PJS_AUDIO_RESULT_OK = 0,
    PJS_AUDIO_RESULT_ARGUMENT = -1,
    PJS_AUDIO_RESULT_I2C = -2,
    PJS_AUDIO_RESULT_CLOCK = -3,
    PJS_AUDIO_RESULT_FIFO = -4,
    PJS_AUDIO_RESULT_NOT_READY = -5,
};

typedef struct {
    uint8_t state;
    uint8_t last_error;
    uint16_t reserved;
    uint32_t codec_writes;
    uint32_t i2c_recoveries;
    uint32_t fifo_timeouts;
    uint32_t tones;
} PjsAudioState;

void pjs_audio_state_init(PjsAudioState *audio);
int pjs_audio_init(PjsAudioState *audio);
int pjs_audio_tone(PjsAudioState *audio);
int pjs_audio_stop(PjsAudioState *audio);
int pjs_audio_resume(PjsAudioState *audio);

/* Main-context codec operations for the separately gated native DMA sink.
 * DMA ownership and queued PCM remain outside this codec driver. */
int pjs_audio_pcm_prepare(PjsAudioState *audio);
int pjs_audio_pcm_mute(PjsAudioState *audio, bool muted);

#endif
