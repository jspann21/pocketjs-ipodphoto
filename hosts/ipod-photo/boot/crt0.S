.syntax unified
.cpu arm7tdmi
.arm

/* ARMv4T startup and Timer1 IRQ ownership for the PP5020 host. */

.section .text.vectors, "ax", %progbits
.global _start
.type _start, %function
_start:
    b reset_handler
    b vector_undef
    b vector_swi
    b vector_prefetch_abort
    b vector_data_abort
    b vector_reserved
    b vector_irq
    b vector_fiq
.size _start, . - _start

#if PJS_BOOTLOADER_HANDOFF
.section .text.handoff, "ax", %progbits
.global pjs_handoff_signature
pjs_handoff_signature:
    .ascii "Rockbox"
    .byte 1
    .space 216
#endif

.section .text.startup, "ax", %progbits
.global reset_handler
.type reset_handler, %function
reset_handler:
    msr cpsr_c, #0xd3

    mov r0, #0x60000000
    ldr r1, [r0]
    and r1, r1, #0xff
    cmp r1, #0x55
    beq cpu_start

    orr r0, r0, #0x7000
    mov r1, #0x80000000
    str r1, [r0, #4]
    nop
    nop
    nop
cop_park:
    b cop_park

cpu_start:
    mov r0, #0x60000000
    orr r0, r0, #0x00007000
    /* A direct RAM handoff transfers only the CPU. Enforce the parked-COP
     * boundary that a two-core reset normally establishes. */
    mov r1, #0x80000000
    str r1, [r0, #4]
    nop
    nop
    nop
    mov r2, #0x00100000
9:
    ldr r1, [r0, #4]
    tst r1, #0x80000000
    bne 8f
    subs r2, r2, #1
    bne 9b
    b pre_remap_hang
8:
    ldr sp, =0x40017c00

    mov r0, #0x60000000
    orr r0, r0, #0x0000c000
    ldr r1, [r0]
    tst r1, #0x1
    beq cache_disable

    mov r3, #0xf0000000
    orr r3, r3, #0x0000f000
    orr r3, r3, #0x44
    ldr r1, [r3]
    orr r1, r1, #0x2
    str r1, [r3]
    mov r2, #0x00100000
cache_clean_wait:
    ldr r1, [r0]
    tst r1, #0x8000
    beq cache_disable
    subs r2, r2, #1
    bne cache_clean_wait
    b pre_remap_hang

cache_disable:
    mov r1, #0
    str r1, [r0]
    nop
    nop
    nop
    nop

    adr r0, reset_handler
    and r1, r0, #0xff000000
    cmp r1, #0
    beq post_remap
    cmp r1, #0x10000000
    bne pre_remap_hang

    adr r0, remap_stub_start
    adr r1, remap_stub_end
    mov r2, #0x40000000
1:
    cmp r0, r1
    ldrlo r3, [r0], #4
    strlo r3, [r2], #4
    blo 1b
    mov pc, #0x40000000

.align 2
remap_stub_start:
    mov r0, #0xf0000000
    orr r0, r0, #0x0000f000
    mov r1, #0x00003e00
    str r1, [r0]
    mov r1, #0x10000000
    orr r1, r1, #0x00000f00
    orr r1, r1, #0x84
    str r1, [r0, #4]
    ldr pc, remap_target
remap_target:
    .word post_remap
remap_stub_end:

pre_remap_hang:
    b pre_remap_hang

post_remap:
    msr cpsr_c, #0xd7
    ldr sp, =0x40002000
    msr cpsr_c, #0xdb
    ldr sp, =0x40004000
    msr cpsr_c, #0xd1
    ldr sp, =0x40006000
    msr cpsr_c, #0xd2
    ldr sp, =0x40008000
    msr cpsr_c, #0xd3
    ldr sp, =0x40017c00

    ldr r0, =__bss_start
    ldr r1, =__bss_end
    mov r2, #0
2:
    cmp r0, r1
    strlo r2, [r0], #4
    blo 2b

    ldr sp, =__stack_top
    bl kernel_main
    mov r0, #0xff
    bl panic_code
3:
    b 3b
.size reset_handler, . - reset_handler

.macro FAULT_VECTOR name, reason, adjust
.global \name
.type \name, %function
\name:
    sub lr, lr, #\adjust
    stmdb sp!, {r0-r12, lr}
    mov r0, #\reason
    mov r1, lr
    mrs r2, spsr
    mov r3, sp
    bl panic_fault
1:
    b 1b
.size \name, . - \name
.endm

FAULT_VECTOR vector_undef,          1, 4
FAULT_VECTOR vector_swi,            2, 4
FAULT_VECTOR vector_prefetch_abort, 3, 4
FAULT_VECTOR vector_data_abort,     4, 8
FAULT_VECTOR vector_reserved,       5, 4

.global vector_irq
.type vector_irq, %function
vector_irq:
    sub lr, lr, #4
    stmdb sp!, {r0-r12, lr}
    mrs r0, spsr
    /* Preserve an 8-byte C ABI boundary. The register frame is 56 bytes;
     * saving SPSR as an eight-byte pair keeps SP aligned for irq_dispatch. */
    stmdb sp!, {r0,r1}
    bl irq_dispatch
    ldmia sp!, {r0,r1}
    msr spsr_cxsf, r0
    ldmia sp!, {r0-r12, lr}
    movs pc, lr
.size vector_irq, . - vector_irq

FAULT_VECTOR vector_fiq,            7, 4
