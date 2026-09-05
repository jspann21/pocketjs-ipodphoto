#include "lcd.h"
#include "platform.h"
#include "pp5020.h"
#include "timer.h"
#include "audio_stream_gate.h"

#ifndef PJS_A1099_LCD_TYPE
/* M9829/P98 60 GB is the old Color/Photo panel family. Use -1 to probe GPIO. */
#define PJS_A1099_LCD_TYPE 0
#endif

#define LCD_STATE_COLD  0x4c434400u
#define LCD_STATE_READY 0x4c43444fu
#define LCD_STATE_ASLEEP 0x4c434453u
#define LCD_MAX_BLOCK_BYTES 0x10000u
#define LCD_BLOCK_PIXELS (LCD_MAX_BLOCK_BYTES / 2u)

static uint32_t lcd_state = LCD_STATE_COLD;
static uint8_t panel_type;

static bool wait_port(void)
{
    uint32_t left = PJS_LCD_POLL_LIMIT;
    while ((PP_LCD2_PORT & PP_LCD2_BUSY_MASK) != 0u) {
        if (--left == 0u) return false;
    }
    return true;
}

static bool wait_block(uint32_t mask)
{
    uint32_t left = PJS_LCD_POLL_LIMIT;
    while ((PP_LCD2_BLOCK_CTRL & mask) == 0u) {
        if (--left == 0u) return false;
    }
    return true;
}

static bool command_data(uint16_t command, uint16_t data)
{
    if ((panel_type & 1u) == 0u) {
        if (!wait_port()) return false;
        PP_LCD2_PORT = PP_LCD2_CMD_MASK | command;
        if (!wait_port()) return false;
        PP_LCD2_PORT = PP_LCD2_CMD_MASK | data;
    } else {
        if (!wait_port()) return false;
        PP_LCD2_PORT = PP_LCD2_CMD_MASK;
        PP_LCD2_PORT = PP_LCD2_CMD_MASK | command;
        if (!wait_port()) return false;
        PP_LCD2_PORT = PP_LCD2_DATA_MASK | (data >> 8);
        PP_LCD2_PORT = PP_LCD2_DATA_MASK | (data & 0xffu);
    }
    return true;
}

static bool initialize_panel(void)
{
    if ((panel_type & 1u) != 0u) return false;
    return command_data(0xefu, 0x0000u) &&
           command_data(0x01u, 0x0000u) &&
           command_data(0x80u, 0x0001u) &&
           command_data(0x10u, 0x000cu) &&
           command_data(0x18u, 0x0006u) &&
           command_data(0x7eu, 0x0004u) &&
           command_data(0x7eu, 0x0005u) &&
           command_data(0x7fu, 0x0001u);
}

static bool claim_bridge(void)
{
    /* Own only the PP5020 LCD device gate. The shared clock-source registers
     * remain at the reversible loader's known-good selection. */
    PP_DEV_EN |= PP_DEV_LCD;
    /* A RAM handoff can stop the previous owner during a synchronous block
     * transfer, leaving BUSY asserted forever. Do not wait on inherited
     * state: cancel the block window and reset the LCD bridge first. */
    PP_LCD2_BLOCK_CONFIG = 0u;
    PP_DEV_RS |= PP_DEV_LCD;
    timer_delay_us(1u);
    PP_DEV_RS &= ~PP_DEV_LCD;
    timer_delay_us(1000u);
    PP_LCD2_BLOCK_CONFIG = 0u;
    return wait_port();
}

static bool setup_region(uint32_t x, uint32_t y, uint32_t width, uint32_t height)
{
    if (width == 0u || height == 0u || x + width > PJS_LCD_WIDTH ||
        y + height > PJS_LCD_HEIGHT) {
        return false;
    }

    uint16_t y0 = (uint16_t)y;
    uint16_t y1 = (uint16_t)(y + height - 1u);
    uint16_t x1 = (uint16_t)((PJS_LCD_WIDTH - 1u) - x);
    uint16_t x0 = (uint16_t)(x1 - width + 1u);

    if ((panel_type & 1u) == 0u) {
        return command_data(0x12u, y0) &&
               command_data(0x13u, x1) &&
               command_data(0x15u, y1) &&
               command_data(0x16u, x0);
    }

    if (!command_data(0x44u, (uint16_t)((y1 << 8) | y0))) return false;
    if (!command_data(0x45u, (uint16_t)((x1 << 8) | x0))) return false;
    if (!command_data(0x21u, (uint16_t)((x1 << 8) | y0))) return false;
    if (!wait_port()) return false;
    PP_LCD2_PORT = PP_LCD2_CMD_MASK;
    PP_LCD2_PORT = PP_LCD2_CMD_MASK | 0x22u;
    return true;
}

static bool transfer_rows(const uint16_t *pixels, uint32_t width,
                          uint32_t rows, uint32_t stride)
{
    if (pixels == 0 || width == 0u || rows == 0u ||
        width > LCD_BLOCK_PIXELS || rows > LCD_BLOCK_PIXELS / width ||
        stride < width || ((width | stride) & 1u) != 0u ||
        (((uintptr_t)pixels) & 3u) != 0u) {
        return false;
    }

    uint32_t byte_count = width * rows * 2u;
    const uint32_t *words = (const uint32_t *)(const void *)pixels;
    uint32_t word_count = width / 2u;
    uint32_t refill_words = 0u;

    PP_LCD2_BLOCK_CTRL = 0x10000080u;
    PP_LCD2_BLOCK_CONFIG = 0xc0010000u | (byte_count - 1u);
    PP_LCD2_BLOCK_CTRL = 0x34000000u;

    for (uint32_t row = 0u; row < rows; ++row) {
        for (uint32_t index = 0u; index < word_count; ++index) {
            /* Keep the native audio producer fed every 2 KiB without
             * calling codec, guest, or rendering code. */
            if ((refill_words++ & 511u) == 0u) pjs_audio_stream_gate_refill();
            if (!wait_block(PP_LCD2_BLOCK_TXOK)) {
                PP_LCD2_BLOCK_CONFIG = 0u;
                return false;
            }
            PP_LCD2_BLOCK_DATA = words[index];
        }
        words += stride / 2u;
    }

    if (!wait_block(PP_LCD2_BLOCK_READY)) {
        PP_LCD2_BLOCK_CONFIG = 0u;
        return false;
    }
    PP_LCD2_BLOCK_CONFIG = 0u;
    return true;
}

static bool present_full(const uint16_t *pixels)
{
    if (!setup_region(0u, 0u, PJS_LCD_WIDTH, PJS_LCD_HEIGHT)) return false;

    uint32_t rows_left = PJS_LCD_HEIGHT;
    const uint16_t *source = pixels;
    while (rows_left != 0u) {
        uint32_t rows = rows_left > 148u ? 148u : rows_left;
        uint32_t count = PJS_LCD_WIDTH * rows;
        if (!transfer_rows(source, PJS_LCD_WIDTH, rows, PJS_LCD_WIDTH)) return false;
        source += count;
        rows_left -= rows;
    }
    return true;
}

static bool present_region(const uint16_t *pixels, const PjsCoreDamageRect *region)
{
    if (region == 0 || region->x0 < 0 || region->y0 < 0 ||
        region->x1 <= region->x0 || region->y1 <= region->y0 ||
        region->x1 > (int32_t)PJS_LCD_WIDTH ||
        region->y1 > (int32_t)PJS_LCD_HEIGHT ||
        (region->x0 & 1) != 0 || (region->x1 & 1) != 0) {
        return false;
    }

    uint32_t x = (uint32_t)region->x0;
    uint32_t y = (uint32_t)region->y0;
    uint32_t width = (uint32_t)(region->x1 - region->x0);
    uint32_t height = (uint32_t)(region->y1 - region->y0);
    if (!setup_region(x, y, width, height)) return false;
    /* The LCD bridge consumes CPU-written pixel pairs, so aligned rows can
     * come directly from the cached framebuffer, as in Rockbox Photo. */
    const uint16_t *source = pixels + (size_t)y * PJS_LCD_WIDTH + x;
    while (height != 0u) {
        uint32_t rows = LCD_BLOCK_PIXELS / width;
        if (rows > height) rows = height;
        if (!transfer_rows(source, width, rows, PJS_LCD_WIDTH)) return false;
        source += (size_t)rows * PJS_LCD_WIDTH;
        height -= rows;
    }
    return true;
}

bool lcd_init(void)
{
#if PJS_A1099_LCD_TYPE < 0
    panel_type = (uint8_t)((PP_GPIOA_INPUT_VAL & 0x02u) |
                           ((PP_GPIOA_INPUT_VAL & 0x10u) >> 4));
#else
    panel_type = (uint8_t)PJS_A1099_LCD_TYPE;
#endif

    if (!claim_bridge() || !initialize_panel()) return false;

    lcd_state = LCD_STATE_READY;
    return true;
}

bool lcd_sleep(void)
{
    if (lcd_state != LCD_STATE_READY || (panel_type & 1u) != 0u ||
        !wait_port()) return false;
    if (!command_data(0xefu, 0x0000u) ||
        !command_data(0x80u, 0x0000u)) return false;
    timer_delay_us(1000u);
    if (!command_data(0x01u, 0x0001u)) return false;
    lcd_state = LCD_STATE_ASLEEP;
    return true;
}

bool lcd_wake(void)
{
    if (lcd_state != LCD_STATE_ASLEEP || !initialize_panel()) return false;
    lcd_state = LCD_STATE_READY;
    return true;
}

bool lcd_present(const uint16_t *pixels, size_t pixel_count)
{
    if (lcd_state != LCD_STATE_READY || pixels == 0 ||
        pixel_count != PJS_FRAME_PIXELS || (((uintptr_t)pixels) & 3u) != 0u) {
        return false;
    }
    return present_full(pixels);
}

bool lcd_present_damage(const uint16_t *pixels, size_t pixel_count,
                        const PjsCoreDamagePlan *damage)
{
    if (lcd_state != LCD_STATE_READY || pixels == 0 || damage == 0 ||
        pixel_count != PJS_FRAME_PIXELS || (((uintptr_t)pixels) & 3u) != 0u ||
        damage->count > PJS_CORE_MAX_DAMAGE_REGIONS) {
        return false;
    }
    if (damage->count == 0u) return true;
    if (damage->full_redraw != 0u) return present_full(pixels);

    for (uint32_t index = 0u; index < damage->count; ++index) {
        if (!present_region(pixels, &damage->regions[index])) return false;
    }
    return true;
}

bool lcd_ready(void)
{
    return lcd_state == LCD_STATE_READY;
}

uint8_t lcd_panel_type(void)
{
    return panel_type;
}

void lcd_clock_snapshot(PjsLcdClockState *state)
{
    if (state == 0) return;
    state->source = PP_CLOCK_SOURCE;
    state->clcd_source = PP_CLCD_CLOCK_SRC;
}

bool lcd_clock_state_matches(const PjsLcdClockState *state)
{
    return state != 0 && state->source == PP_CLOCK_SOURCE &&
           state->clcd_source == PP_CLCD_CLOCK_SRC;
}

uint16_t lcd_rgb565_swapped(uint8_t r, uint8_t g, uint8_t b)
{
    uint16_t native = (uint16_t)(((uint16_t)(r & 0xf8u) << 8) |
                                 ((uint16_t)(g & 0xfcu) << 3) |
                                 ((uint16_t)b >> 3));
    return (uint16_t)((native << 8) | (native >> 8));
}
