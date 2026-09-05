#include "lcd.h"
#include "panic.h"
#include "platform.h"

PjsCrashRecord pjs_crash_record __attribute__((section(".noinit")));
static uint16_t panic_frame[PJS_FRAME_PIXELS] __attribute__((aligned(4)));

static void fill(uint16_t color)
{
    for (size_t index = 0; index < PJS_FRAME_PIXELS; ++index) {
        panic_frame[index] = color;
    }
}

static void bit_columns(uint32_t value, uint32_t y, uint16_t on, uint16_t off)
{
    for (uint32_t bit = 0u; bit < 32u; ++bit) {
        uint16_t color = ((value >> bit) & 1u) != 0u ? on : off;
        uint32_t x0 = 6u + bit * 6u;
        for (uint32_t yy = y; yy < y + 12u; ++yy) {
            for (uint32_t xx = x0; xx < x0 + 4u; ++xx) {
                panic_frame[yy * PJS_LCD_WIDTH + xx] = color;
            }
        }
    }
}

static void show_record(void)
{
    if (!lcd_ready()) return;
    uint16_t red = lcd_rgb565_swapped(180u, 0u, 0u);
    uint16_t white = lcd_rgb565_swapped(255u, 255u, 255u);
    uint16_t black = lcd_rgb565_swapped(0u, 0u, 0u);
    uint16_t yellow = lcd_rgb565_swapped(255u, 220u, 0u);
    fill(red);
    bit_columns(pjs_crash_record.reason, 22u, white, black);
    bit_columns(pjs_crash_record.pc, 66u, yellow, black);
    bit_columns(pjs_crash_record.spsr, 110u, white, black);
    (void)lcd_present(panic_frame, PJS_FRAME_PIXELS);
}

void panic_fault(uint32_t reason, uint32_t pc, uint32_t spsr,
                 const uint32_t *registers)
{
    pjs_crash_record.magic = PJS_CRASH_MAGIC;
    pjs_crash_record.reason = reason;
    pjs_crash_record.pc = pc;
    pjs_crash_record.spsr = spsr;
    for (uint32_t index = 0u; index < 14u; ++index) {
        pjs_crash_record.regs[index] = registers != 0 ? registers[index] : 0u;
    }
    volatile uint32_t *retained =
        (volatile uint32_t *)(uintptr_t)PJS_RETAINED_CRASH_ADDRESS;
    const uint32_t *record_words = (const uint32_t *)&pjs_crash_record;
    for (uint32_t index = 0u; index < sizeof(PjsCrashRecord) / 4u; ++index) {
        retained[index] = record_words[index];
    }
    show_record();
    for (;;) {
        __asm__ volatile("nop");
    }
}

void panic_code(uint32_t reason)
{
    panic_fault(reason, 0u, 0u, 0);
}
