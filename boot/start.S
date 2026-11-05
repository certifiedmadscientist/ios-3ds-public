/*
 * start.S — ARM11 bare-metal entry point for iOS 3DS FIRM payload
 *
 * When Luma3DS chainloads a FIRM, ARM11 enters at the specified
 * entry point in SVC mode.
 */

.section .text.start, "ax"
.arm
.align 4

/*
 * Fill bottom screen with solid BGR color for boot diagnostics.
 * Reads FB address from GPU register. Clobbers r9-r12.
 */
.macro DIAG_COLOR b, g, r
    ldr     r9, =0x10400568
    ldr     r9, [r9]
    ldr     r10, =230400
77:
    mov     r11, #\b
    strb    r11, [r9], #1
    mov     r11, #\g
    strb    r11, [r9], #1
    mov     r11, #\r
    strb    r11, [r9], #1
    subs    r10, r10, #3
    bgt     77b
.endm

.global _start
_start:
    /* Stage 4: CYAN — entered start.S in FCRAM */
    DIAG_COLOR 0xFF, 0xFF, 0x00

    /* ── 1. Disable IRQ and FIQ interrupts ── */
    cpsid   if

    /* ── 2. Set up exception-mode stacks ── */

    /* ABT mode stack (used by the HTL data-abort handler) */
    msr     cpsr_c, #0xD7              @ ABT mode | I=1 F=1
    ldr     sp, =0x27FC0000

    /* UND mode stack (for undefined instruction emulation) */
    msr     cpsr_c, #0xDB              @ UND mode | I=1 F=1
    ldr     sp, =0x27FA0000

    /* IRQ mode stack (for future IRQ routing) */
    msr     cpsr_c, #0xD2              @ IRQ mode | I=1 F=1
    ldr     sp, =0x27FB0000

    /* Return to SVC mode */
    msr     cpsr_c, #0xD3              @ SVC mode | I=1 F=1
    ldr     sp, =0x28000000

    /* Stage 5: MAGENTA — stacks set up */
    DIAG_COLOR 0xFF, 0x00, 0xFF

    /* ── 3. Invalidate caches (ARMv6 / ARM1176JZF-S) ── */
    mov     r0, #0
    mcr     p15, 0, r0, c7, c5, 0      @ Invalidate entire I-cache
    mcr     p15, 0, r0, c7, c14, 0     @ Clean + Invalidate entire D-cache
    mcr     p15, 0, r0, c7, c10, 4     @ DSB
    mcr     p15, 0, r0, c7, c5, 4      @ ISB

    /* Stage 6: YELLOW — caches done */
    DIAG_COLOR 0x00, 0xFF, 0xFF

    /* ── 4. Disable MMU if enabled ── */
    mrc     p15, 0, r0, c1, c0, 0      @ Read SCTLR
    bic     r0, r0, #1                  @ Clear M bit (MMU enable)
    bic     r0, r0, #(1 << 2)          @ Clear C bit (data cache)
    bic     r0, r0, #(1 << 12)         @ Clear I bit (instruction cache)
    mcr     p15, 0, r0, c1, c0, 0      @ Write SCTLR
    mcr     p15, 0, r0, c7, c5, 4      @ ISB
    mcr     p15, 0, r0, c7, c10, 4     @ DSB

    /* ── 5. Early MMU: we run at PA 0x21800000 but are linked at VA
     * 0xC0800000.  Build a flat table at PA 0x27F00000: identity for
     * everything (IO 0x10000000-0x1FFFFFFF as device, the rest normal
     * non-cacheable) plus 0xC0000000-0xC6FFFFFF -> 0x21000000.  Then enable
     * translation (caches stay off) and jump to the link address.
     * mmu_init() replaces this table with the real one. */
    ldr     r0, =0x27F00000
    mov     r1, #0                      @ section index
    ldr     r2, =0x00001C02             @ normal NC: AP=11 TEX=001 C=0 B=0
    ldr     r3, =0x00000C06             @ device:    AP=11 TEX=000 C=0 B=1
.Lel1_loop:
    mov     r4, r1, lsl #20
    sub     r5, r1, #0x100
    cmp     r5, #0x100                  @ 0x100 <= idx < 0x200 -> device
    orrlo   r4, r4, r3
    orrhs   r4, r4, r2
    str     r4, [r0, r1, lsl #2]
    add     r1, r1, #1
    cmp     r1, #0x1000
    bne     .Lel1_loop

    ldr     r1, =0xC00                  @ kernel linear map, 112 MB
    ldr     r4, =0x21000000
.Lel1_kern:
    orr     r5, r4, r2
    str     r5, [r0, r1, lsl #2]
    add     r4, r4, #0x00100000
    add     r1, r1, #1
    cmp     r1, #0xC70
    bne     .Lel1_kern

    mcr     p15, 0, r0, c2, c0, 0      @ TTBR0 (no walk-cache attributes)
    mov     r1, #0
    mcr     p15, 0, r1, c2, c0, 2      @ TTBCR = 0
    mov     r1, #3
    mcr     p15, 0, r1, c3, c0, 0      @ DACR: domain 0 manager
    mov     r1, #0
    mcr     p15, 0, r1, c8, c7, 0      @ TLBIALL
    mcr     p15, 0, r1, c7, c10, 4     @ DSB
    mrc     p15, 0, r1, c1, c0, 0
    orr     r1, r1, #1                  @ M
    mcr     p15, 0, r1, c1, c0, 0
    mov     r1, #0
    mcr     p15, 0, r1, c7, c5, 4      @ ISB
    ldr     pc, =.Lhigh                 @ continue at the link address
.Lhigh:

    /* ── 6. Zero out BSS ── */
    ldr     r0, =__bss_start
    ldr     r1, =__bss_end
    mov     r2, #0
.Lbss_loop:
    cmp     r0, r1
    strlt   r2, [r0], #4
    blt     .Lbss_loop

    /* Stage 7: WHITE — about to call main() */
    DIAG_COLOR 0xFF, 0xFF, 0xFF

    /* ── 7. Branch to C entry point ── */
    bl      main

    /* If main() returns, hang */
.Lhang:
    wfi
    b       .Lhang

.pool
