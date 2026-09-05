OUTPUT_ARCH(arm)
ENTRY(_start)

MEMORY {
    SDRAM (rwx) : ORIGIN = 0x00000000, LENGTH = 32M
}

SECTIONS {
    . = ORIGIN(SDRAM);

    .text : ALIGN(4) {
        KEEP(*(.text.vectors))
        KEEP(*(.text.handoff))
        KEEP(*(.text.startup))
        *(.text*)
        *(.rodata*)
        *(.glue_7)
        *(.glue_7t)
        . = ALIGN(4);
    } > SDRAM

    .data : ALIGN(4) {
        *(.data*)
        . = ALIGN(4);
    } > SDRAM

    __image_end = .;

    .bss (NOLOAD) : ALIGN(4) {
        __bss_start = .;
        *(.bss*)
        *(COMMON)
        . = ALIGN(4);
        __bss_end = .;
    } > SDRAM

    .noinit (NOLOAD) : ALIGN(16) {
        *(.noinit*)
        . = ALIGN(16);
    } > SDRAM

    .stack (NOLOAD) : ALIGN(16) {
        __stack_bottom = .;
        . += 256K;
        __stack_top = .;
    } > SDRAM

    __heap_start = ALIGN(., 16);
    /* The final page is a cross-image crash mailbox.  Preserve the physical
     * RAM ceiling for image tooling while giving the allocator a lower bound. */
    __heap_end = ORIGIN(SDRAM) + LENGTH(SDRAM) - 0x1000;
    __ram_end = ORIGIN(SDRAM) + LENGTH(SDRAM);

    /DISCARD/ : {
        *(.ARM.attributes)
        *(.ARM.exidx*)
        *(.ARM.extab*)
        *(.comment)
        *(.eh_frame*)
        *(.note*)
    }
}

ASSERT(_start == ORIGIN(SDRAM), "A1099 image entry must be offset zero")
ASSERT(__heap_start < __heap_end, "A1099 static image exhausted SDRAM")
ASSERT(__heap_end == 0x01fff000, "retained crash mailbox address changed")
ASSERT(__image_end < 0x01000000, "probe image unexpectedly exceeds 16 MiB")
