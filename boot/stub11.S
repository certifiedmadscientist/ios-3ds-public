/*
 * stub11.S — ARM11 trampoline with diagnostic color flash.
 * Bottom screen color = boot stage reached.
 */

.section .text, "ax"
.arm
.align 4

/* Fill bottom screen with BGR color (r0=B, r1=G, r2=R) */
/* Clobbers r3-r5 */
.macro FILL_BOT b, g, r
    ldr     r3, =0x10400568
    ldr     r3, [r3]
    ldr     r4, =230400
99:
    mov     r5, #\b
    strb    r5, [r3], #1
    mov     r5, #\g
    strb    r5, [r3], #1
    mov     r5, #\r
    strb    r5, [r3], #1
    subs    r4, r4, #3
    bgt     99b
.endm

.macro DELAY
    ldr     r0, =0x200000
98: subs    r0, r0, #1
    bne     98b
.endm

.global _stub11_start
_stub11_start:
    cpsid   aif, #0x13

    /* Disable MMU/caches */
    mrc     p15, 0, r0, c1, c0, 0
    bic     r0, r0, #(1 << 0)
    bic     r0, r0, #(1 << 2)
    bic     r0, r0, #(1 << 12)
    mcr     p15, 0, r0, c1, c0, 0

    mov     r0, #0
    mcr     p15, 0, r0, c7, c5, 0      @ ICIALLU
    mcr     p15, 0, r0, c7, c14, 0     @ Clean+Inv D-cache
    mcr     p15, 0, r0, c8, c7, 0      @ TLBIALL
    mcr     p15, 0, r0, c7, c10, 4     @ DSB
    mcr     p15, 0, r0, c7, c5, 4      @ ISB

    /* Stage 1: RED — ARM11 entered stub */
    FILL_BOT 0x00, 0x00, 0xFF
    DELAY

    /* Stage 2: GREEN — about to read FCRAM */
    ldr     r6, =0x21800000
    ldr     r7, [r6]                    @ test read
    FILL_BOT 0x00, 0xFF, 0x00
    DELAY

    /* Stage 3: BLUE — about to jump to FCRAM */
    FILL_BOT 0xFF, 0x00, 0x00
    DELAY

    /* Jump to bootloader */
    ldr     pc, =0x21800000

.pool
