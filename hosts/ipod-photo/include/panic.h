#ifndef POCKETJS_IPOD_PHOTO_PANIC_H
#define POCKETJS_IPOD_PHOTO_PANIC_H

#include <stdint.h>

typedef struct {
    uint32_t magic;
    uint32_t reason;
    uint32_t pc;
    uint32_t spsr;
    uint32_t regs[14];
} PjsCrashRecord;

/* Keep a second crash record in the final reserved SDRAM page.  Unlike the
 * image-local .noinit object, this address does not move when code or BSS
 * changes, so a small recovery image can retrieve a fault after reboot. */
#define PJS_RETAINED_CRASH_ADDRESS 0x01fff000u
#define PJS_RETAINED_CRASH_BYTES   0x00001000u
#define PJS_CRASH_MAGIC            0x504a4352u /* PJCR */

void panic_fault(uint32_t reason, uint32_t pc, uint32_t spsr,
                 const uint32_t *registers) __attribute__((noreturn));
void panic_code(uint32_t reason) __attribute__((noreturn));

#endif
