#ifndef POCKETJS_IPOD_PHOTO_AUDIO_STREAM_GATE_H
#define POCKETJS_IPOD_PHOTO_AUDIO_STREAM_GATE_H

#include <stdbool.h>
#include <stdint.h>
#include "audio.h"

/* Native diagnostic producer only; not the portable audio module. */
int pjs_audio_stream_gate_start(PjsAudioState *audio);
void pjs_audio_stream_gate_tick(uint32_t now);
/* Main-context cooperative producer service. No codec, rendering, guest, or
 * diagnostic-state transitions; safe between units of display/guest work. */
void pjs_audio_stream_gate_refill(void);
/* Mixer gate only: main-loop backpressure, never called from an IRQ. */
bool pjs_audio_stream_gate_needs_service(void);
int pjs_audio_stream_gate_cancel(void);
bool pjs_audio_stream_gate_active(void);
bool pjs_audio_stream_gate_status(uint32_t *mode, uint32_t *error);

#endif
