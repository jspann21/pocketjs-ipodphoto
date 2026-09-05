.syntax unified
.cpu arm7tdmi
.arm

/* Copied to IRAM before use. r0 is a validated flat image in SDRAM and r1 is
 * its four-byte-aligned length. The destination begins below the source, so
 * a forward memmove is safe even when the ranges overlap. */
.section .text.chainload, "ax", %progbits
.align 2
.global pjs_chainload_stub_start
.global pjs_chainload_stub_end
.type pjs_chainload_stub_start, %function
pjs_chainload_stub_start:
    msr cpsr_c, #0xd3
    mov r2, #0
1:
    ldr r3, [r0], #4
    str r3, [r2], #4
    subs r1, r1, #4
    bne 1b
    nop
    nop
    nop
    mov pc, #0
pjs_chainload_stub_end:
.size pjs_chainload_stub_start, . - pjs_chainload_stub_start
