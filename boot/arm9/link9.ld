/*
 * link9.ld -- Linker script for the ARM9 SD-loader stub.
 *
 * Code and data are placed in ARM9 internal SRAM at 0x08000000.
 * The FIRM loader copies the ARM9 section here before entry.
 */

ENTRY(_start9)

MEMORY {
    sram : ORIGIN = 0x08000000, LENGTH = 256K
}

SECTIONS {
    .text : {
        *(.text.start)
        *(.text*)
    } > sram

    .rodata : { *(.rodata*) } > sram
    .data   : { *(.data*)   } > sram

    .bss : {
        __bss9_start = .;
        *(.bss*) *(COMMON)
        __bss9_end = .;
    } > sram

    /DISCARD/ : { *(.ARM.*) *(.comment) *(.note*) }
}
