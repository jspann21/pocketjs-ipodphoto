#include "backlight.h"
#include "pp5020.h"

static uint8_t current_brightness = 160u;
static bool current_enabled;

void backlight_set(uint8_t brightness)
{
    current_brightness = brightness == 0u ? 1u : brightness;
    if (current_enabled) {
        PP_PWM_BACKLIGHT = 0x80000000u | ((uint32_t)current_brightness << 16);
    }
}

void backlight_enable(bool enabled)
{
    if (enabled) {
        PP_PWM_BACKLIGHT = 0x80000000u | ((uint32_t)current_brightness << 16);
        pp_gpio_set((volatile uint32_t *)(uintptr_t)0x6000d824u, 0x08u);
    } else {
        PP_PWM_BACKLIGHT = 0x80000000u;
        pp_gpio_clear((volatile uint32_t *)(uintptr_t)0x6000d824u, 0x08u);
    }
    current_enabled = enabled;
}

bool backlight_enabled(void)
{
    return current_enabled;
}

uint8_t backlight_brightness(void)
{
    return current_brightness;
}

void backlight_suspend(void)
{
    backlight_enable(false);
}

void backlight_resume(void)
{
    backlight_enable(true);
}

void backlight_init(void)
{
    /* B2/B3 are owned by the backlight path; B3 gates the panel light. */
    pp_gpio_set((volatile uint32_t *)(uintptr_t)0x6000d804u, 0x0cu);
    pp_gpio_set((volatile uint32_t *)(uintptr_t)0x6000d824u, 0x08u);
    PP_GPO32_ENABLE &= ~0x02000000u;
    PP_DEV_EN |= PP_DEV_PWM;
    current_enabled = true;
    backlight_set(current_brightness);
}
