#ifndef PJS_AUDIO_CLOCK_H
#define PJS_AUDIO_CLOCK_H

/* Shared PP5020 clock transition, with audio DMA stopped and COP parked. */
/* Establish the standalone CPU baseline before peripheral initialization.
 * This does not acquire an audio lease and is not undone by app teardown. */
int pjs_cpu_clock_init(void);
int pjs_audio_clock_acquire(void);
int pjs_audio_clock_release(void);

#endif
