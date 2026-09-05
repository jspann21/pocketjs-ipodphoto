#ifndef POCKETJS_IPOD_PHOTO_PLATFORM_H
#define POCKETJS_IPOD_PHOTO_PLATFORM_H

#include <stddef.h>
#include <stdint.h>

#define PJS_LCD_WIDTH 220u
#define PJS_LCD_HEIGHT 176u
#define PJS_FRAME_PIXELS ((size_t)PJS_LCD_WIDTH * (size_t)PJS_LCD_HEIGHT)

#define PJS_CPU_ID 0x55u
#define PJS_COP_ID 0xaau

/* A1099 / P98 / M9829 has 32 MiB of SDRAM and 96 KiB of usable IRAM. */
#define PJS_SDRAM_BYTES (32u * 1024u * 1024u)
#define PJS_IRAM_BASE 0x40000000u
#define PJS_IRAM_BYTES (96u * 1024u)

/* Probe presentation rate. The PocketJS runtime will keep 60 Hz simulation
 * semantics while presenting only when the LCD is ready and content is dirty. */
#define PJS_PROBE_FRAME_US 33333u
#define PJS_TIMER_IRQ_US 1000u
#define PJS_RENDER_GAP_US 50000u
#define PJS_HEAP_TOP_GUARD (64u * 1024u)

/* Conservative timeouts: every hardware wait must terminate. */
#define PJS_LCD_POLL_LIMIT 1000000u
#define PJS_WHEEL_POLL_LIMIT 10000u
#define PJS_CACHE_POLL_LIMIT 1000000u

#endif
