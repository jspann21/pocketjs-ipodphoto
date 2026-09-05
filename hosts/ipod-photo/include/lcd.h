#ifndef POCKETJS_IPOD_PHOTO_LCD_H
#define POCKETJS_IPOD_PHOTO_LCD_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "core_bridge.h"

bool lcd_init(void);
bool lcd_sleep(void);
bool lcd_wake(void);
bool lcd_present(const uint16_t *pixels, size_t pixel_count);
bool lcd_present_damage(const uint16_t *pixels, size_t pixel_count,
                        const PjsCoreDamagePlan *damage);
bool lcd_ready(void);
uint8_t lcd_panel_type(void);
uint16_t lcd_rgb565_swapped(uint8_t r, uint8_t g, uint8_t b);

typedef struct {
    uint32_t source;
    uint32_t clcd_source;
} PjsLcdClockState;

void lcd_clock_snapshot(PjsLcdClockState *state);
bool lcd_clock_state_matches(const PjsLcdClockState *state);

#endif
