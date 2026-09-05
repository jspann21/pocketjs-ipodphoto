#ifndef POCKETJS_IPOD_PHOTO_AUDIO_PCM_H
#define POCKETJS_IPOD_PHOTO_AUDIO_PCM_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define PJS_AUDIO_PCM_POLL_BYTES 64u

enum {
    PJS_AUDIO_PCM_OK = 0,
    PJS_AUDIO_PCM_ERR_ARGUMENT = -1,
    PJS_AUDIO_PCM_ERR_ENGINE = -2,
    PJS_AUDIO_PCM_ERR_CONTROL = -3,
    PJS_AUDIO_PCM_ERR_FAULT = -4,
};

int pjs_audio_pcm_create_stream(uint32_t sample_rate, uint32_t channels);
void pjs_audio_pcm_destroy_stream(int handle);
uint32_t pjs_audio_pcm_write(int handle, const int16_t *pcm, uint32_t frames);
uint32_t pjs_audio_pcm_write_bytes(int handle, const uint8_t *bytes, size_t length);
void pjs_audio_pcm_play(int handle);
void pjs_audio_pcm_pause(int handle);
void pjs_audio_pcm_stop(int handle);
void pjs_audio_pcm_set_volume(int handle, double volume);
void pjs_audio_pcm_end_stream(int handle);

/* Freeze native facts at the guest tick boundary. poll() drains only queued
 * boundary events; facts that arrive during the JS turn wait for the next
 * begin_tick(). */
void pjs_audio_pcm_begin_tick(void);
const char *pjs_audio_pcm_poll(void);

/* Cooperative native service. Safe from main-thread QuickJS interrupt checks
 * and Rust raster strip yield points: never calls JS/UI and never allocates. */
void pjs_audio_pcm_service(void);
bool pjs_audio_pcm_needs_service(void);
bool pjs_audio_pcm_active(void);
/* Engine ownership remains active for paused/ended live handles.  busy() is
 * the lifecycle query: true only while a source is playing, draining, or the
 * shared transport still has work to retire. */
bool pjs_audio_pcm_busy(void);
int pjs_audio_pcm_suspend(void);
int pjs_audio_pcm_resume(void);
int pjs_audio_pcm_reset(void);
uint32_t pjs_audio_pcm_last_error(void);

/* Compatibility yield symbol used by the already-qualified cooperative-audio
 * Rust raster feature and existing main-loop refill sites. The diagnostic
 * stream gate is not linked in an AUDIO_PCM_GATE build. */
void pjs_audio_stream_gate_refill(void);

#endif
