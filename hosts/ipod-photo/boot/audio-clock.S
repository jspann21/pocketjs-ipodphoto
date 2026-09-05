.syntax unified
.cpu arm7tdmi
.arm

/* Position-independent PP5020 clock transition, copied to reserved IRAM.
 * r0 = PLL control, r1 = memory timing, r2 = clock source, r3 = INIT_PLL.
 * All stack accesses precede/follow the complete transition; no SDRAM
 * accesses, external calls, or absolute code addresses occur inside it.
 * COP is already parked by reset and must NEVER be awakened here.
 * Register sequence: Rockbox system-pp502x.c; PP5020 unlock/rewrite/500 us
 * also corroborated by RetailOS 5.1.2.1 soso file offsets 0x78c..0x7a8.
 */
.section .text.audio_clock_stub, "ax", %progbits
.balign 4
.global pjs_audio_clock_stub_start
.global pjs_audio_clock_stub_end
.type pjs_audio_clock_stub_start, %function
pjs_audio_clock_stub_start:
    push {r4-r10, lr}
    mrs r10, cpsr
    orr r7, r10, #0xc0
    msr cpsr_c, r7
    ldr r4, =0x60006000
    mov r5, #0x70000000
    ldr r6, =0x60007000
    ldr r9, [r6]
    ldr r7, [r5, #0x20]
    orr r7, r7, #0x40000000
    str r7, [r5, #0x20]
    ldr r7, [r4, #0x34]
    orr r7, r7, #0x88000000
    str r7, [r4, #0x34]

    ldr r7, =0x48000003
    str r7, [r6]
    nop
    ldr r7, =0x20002222
    str r7, [r4, #0x20]
    ldr r7, =0x00000303
    str r7, [r5, #0x34]
    ldr r7, =0x4800001f
    str r7, [r6]
    nop

    orr r7, r0, #0x80000000
    str r7, [r4, #0x34]
    ldr r8, =0x0000d19b
    str r8, [r4, #0x3c]
    str r7, [r4, #0x34]
    /* USEC_TIMER is independent of the CPU PLL. Fixed settling delay,
     * never the unbounded lock-bit polling used on other PP variants. */
    ldr r8, [r4, #-0xff0]
1:
    ldr r7, [r4, #-0xff0]
    sub r7, r7, r8
    cmp r7, #500
    blo 1b

    ldr r7, =0x48000003
    str r7, [r6]
    nop
    str r1, [r5, #0x34]
    str r2, [r4, #0x20]
    ldr r7, =0x4800001f
    str r7, [r6]
    nop
    /* Do not rewrite an active PLL after selecting it. Only turn it off
     * here when restoring an inherited non-PLL clock configuration. */
    tst r0, #0x80000000
    streq r0, [r4, #0x34]
    ldr r7, [r5, #0x20]
    bic r7, r7, #0x40000000
    orr r7, r7, r3
    str r7, [r5, #0x20]
    str r9, [r6]
    nop
    msr cpsr_c, r10
    pop {r4-r10, lr}
    bx lr
.ltorg
.size pjs_audio_clock_stub_start, . - pjs_audio_clock_stub_start
pjs_audio_clock_stub_end:
