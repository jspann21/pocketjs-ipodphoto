#ifndef POCKETJS_IPOD_PHOTO_BACKLIGHT_H
#define POCKETJS_IPOD_PHOTO_BACKLIGHT_H

#include <stdbool.h>
#include <stdint.h>

void backlight_init(void);
void backlight_set(uint8_t brightness);
void backlight_enable(bool enabled);
bool backlight_enabled(void);
uint8_t backlight_brightness(void);
void backlight_suspend(void);
void backlight_resume(void);

#endif
