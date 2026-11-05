/*
 * start9.S -- ARM9 bare-metal entry point for iOS 3DS FIRM payload
 *
 * The ARM946E-S (ARMv5TE) enters here after the FIRM loader copies
 * this code to ARM9 internal SRAM at 0x08000000.
 */

    .section .text.start, "ax"
    .arm
    .align  4

    .global _start9
_start9:
    /* -- 1. SVC mode, IRQ and FIQ disabled -- */
    msr     cpsr_c, #0xD3          @ SVC | I=1 F=1

    /* -- 2. Disable MPU and caches -- */
    mrc     p15, 0, r0, c1, c0, 0
    bic     r0, r0, #(1 << 0)     @ M  -- MPU off
    bic     r0, r0, #(1 << 2)     @ C  -- D-cache off
    bic     r0, r0, #(1 << 12)    @ I  -- I-cache off
    mcr     p15, 0, r0, c1, c0, 0

    /* -- 3. Invalidate caches and drain write buffer -- */
    mov     r0, #0
    mcr     p15, 0, r0, c7, c5, 0 @ Invalidate entire I-cache
    mcr     p15, 0, r0, c7, c6, 0 @ Invalidate entire D-cache
    mcr     p15, 0, r0, c7, c10, 4 @ Drain write buffer (DSB)

    /* -- 4. Set up stack at top of ARM9 SRAM -- */
    ldr     sp, =0x08040000

    /* -- 5. Zero BSS -- */
    ldr     r0, =__bss9_start
    ldr     r1, =__bss9_end
    mov     r2, #0
.Lbss9_loop:
    cmp     r0, r1
    strlt   r2, [r0], #4
    blt     .Lbss9_loop

    /* -- 6. Jump to C entry -- */
    bl      arm9_main

    /* If arm9_main returns, halt */
.Lhalt9:
    mov     r0, #0
    mcr     p15, 0, r0, c7, c0, 4 @ Wait for interrupt (ARM946E-S)
    b       .Lhalt9

    .pool
