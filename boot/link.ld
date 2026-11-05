ENTRY(_start)
/* Linked in the kernel's static linear map (see iowin.h): Luma loads the
 * image at PA 0x21800000 and start.S turns the MMU on and jumps here, so the
 * same addresses stay valid after XNU installs its own page tables. */
MEMORY {
    img : ORIGIN = 0xC0800000, LENGTH = 4M
}
SECTIONS {
    .text : {
        *(.text.start)
        *(.text*)
    } > img

    .rodata : { *(.rodata*) } > img
    .data   : { *(.data*)   } > img

    /* Exception vector table — page-aligned so the MMU can map a
       single 4KB page to 0xFFFF0000 (high vectors). */
    .vectors : ALIGN(4096) {
        __vectors_start = .;
        *(.vectors)
        __vectors_end = .;
    } > img

    /* Exception handlers that run under XNU's page tables. */
    .kern_htl : ALIGN(4096) {
        __kern_htl_start = .;
        *(.kern_htl)
        *(.kern_htl.data)
        __kern_htl_end = .;
    } > img

    .bss (NOLOAD) : ALIGN(4) {
        __bss_start = .;
        *(.bss*) *(COMMON)
        __bss_end = .;
    } > img

    __img_end = .;

    /DISCARD/ : { *(.ARM.*) *(.comment) *(.note*) }
}
