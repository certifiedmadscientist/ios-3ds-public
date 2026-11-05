/*
 * fb_trap2.S — Mach trap handler for 3DS framebuffer blit (v2)
 *
 * Called from fleh_swi when r12 = -10.
 * r0 = pointer to arm_saved_state (user registers).
 *
 * User calling convention:
 *   r0 = command:
 *     0 = fb_fill_rect(color_bgr, x, y, w, h)
 *         r1 = 0x00RRGGBB, r2 = x, r3 = y, r4 = w, r5 = h
 *     1 = fb_pixel(x, y, color_bgr)
 *         r1 = x, r2 = y, r3 = 0x00RRGGBB
 *     2 = fb_get_info() -> returns screen width in r0
 *
 * v2 changes:
 *   - Lazily maps VRAM at 0xBE100000 by writing to the kernel's
 *     L1 page table (found via TTBR1). Survives XNU's pmap_bootstrap.
 *   - Targets VRAM phys 0x18300000 (matches GPU LCD register).
 *
 * 3DS top screen: 400x240, RGB8, column-major.
 * Pixel (x,y): offset = ((239-y) + x*240) * 3, bytes B,G,R
 */

    .syntax unified
    .arm
    .section .text

    .global _fb_trap_handler
_fb_trap_handler:
    push    {r4-r11, lr}
    mov     r11, r0             @ r11 = saved_state pointer

    /* Check if FB is already mapped (cached_fb_va != 0) */
    adr     r0, cached_fb_va
    ldr     r10, [r0]
    cmp     r10, #0
    bne     .Lcached

    /* ── First call: map VRAM into kernel page table ────────────── */

    /* Read TTBR1 (kernel L1 page table physical address) */
    mrc     p15, 0, r1, c2, c0, 1
    bic     r1, r1, #0x3F00
    bic     r1, r1, #0x00FF     @ r1 = L1 phys base (16KB aligned)

    /* Convert physical → kernel VA:
     * kva = phys - physBase + virtBase
     *     = phys - 0x21000000 + 0xC0000000
     *     = phys + 0x9F000000 */
    ldr     r2, phys_to_kva
    add     r1, r1, r2          @ r1 = L1 table kernel VA

    /* Write section descriptor at L1[0xBE1]:
     * offset = 0xBE1 * 4 = 0x2F84
     * descriptor = 0x18300000 | SECTION_UNCACHED
     * SECTION_UNCACHED = L1_SECTION(0x2) | TEX(1)(0x1000) | AP(3)(0xC00)
     *                  = 0x1C02 */
    ldr     r2, l1_index_off
    add     r3, r1, r2          @ r3 = &L1[0xBE1]
    ldr     r4, vram_section
    str     r4, [r3]            @ L1[0xBE1] = VRAM section desc

    /* Invalidate TLB for VA 0xBE100000 */
    ldr     r0, fb_base
    mcr     p15, 0, r0, c8, c7, 1    @ TLBIMVA
    mov     r1, #0
    mcr     p15, 0, r1, c7, c10, 4   @ DSB
    mcr     p15, 0, r1, c7, c5, 4    @ ISB

    /* Cache the mapped VA */
    ldr     r10, fb_base
    adr     r0, cached_fb_va
    str     r10, [r0]

.Lcached:
    /* r10 = FB base VA (0xBE100000)
     * r11 = saved_state pointer */

    ldr     r4, [r11, #0]       @ user_r0 = command
    ldr     r5, [r11, #4]       @ user_r1
    ldr     r6, [r11, #8]       @ user_r2
    ldr     r7, [r11, #12]      @ user_r3
    ldr     r8, [r11, #16]      @ user_r4
    ldr     r9, [r11, #20]      @ user_r5

    cmp     r4, #0
    beq     cmd_fill_rect
    cmp     r4, #1
    beq     cmd_pixel
    cmp     r4, #2
    beq     cmd_get_info

    /* Unknown command → return -1 */
    mvn     r0, #0
    str     r0, [r11, #0]
    pop     {r4-r11, pc}

cmd_get_info:
    mov     r0, #400
    str     r0, [r11, #0]
    pop     {r4-r11, pc}

/*
 * cmd_pixel: write one pixel
 *   r5 = x, r6 = y, r7 = color (0x00RRGGBB)
 */
cmd_pixel:
    cmp     r5, #400
    bhs     .pixel_skip
    cmp     r6, #240
    bhs     .pixel_skip

    /* offset = ((239-y) + x*240) * 3 */
    rsb     r0, r6, #239        @ 239 - y
    mov     r1, #240
    mla     r0, r5, r1, r0      @ x*240 + (239-y)
    add     r1, r0, r0, lsl #1  @ *3
    add     r1, r10, r1          @ FB_BASE + offset

    /* Write B, G, R */
    strb    r7, [r1, #0]         @ B (low byte of color)
    lsr     r0, r7, #8
    strb    r0, [r1, #1]         @ G
    lsr     r0, r7, #16
    strb    r0, [r1, #2]         @ R

.pixel_skip:
    mov     r0, #0
    str     r0, [r11, #0]
    pop     {r4-r11, pc}

/*
 * cmd_fill_rect: fill rectangle with solid color
 *   r5 = color (0x00RRGGBB), r6 = sx, r7 = sy, r8 = w, r9 = h
 */
cmd_fill_rect:
    /* Compute end x/y, clamp to screen */
    add     r0, r6, r8          @ sx + w
    cmp     r0, #400
    movhi   r8, #400
    subhi   r8, r8, r6
    add     r0, r7, r9          @ sy + h
    cmp     r0, #240
    movhi   r9, #240
    subhi   r9, r9, r7

    /* Extract color: B=low, G=mid, R=high */
    and     r4, r5, #0xFF       @ B
    lsr     r0, r5, #8
    and     r3, r0, #0xFF       @ G
    lsr     r0, r5, #16
    and     r2, r0, #0xFF       @ R
    /* r2=R, r3=G, r4=B — these survive the loop */

    /* end_x = sx + w, end_y = sy + h */
    add     r8, r6, r8          @ end_x (already clamped w)
    add     r9, r7, r9          @ end_y

    /* Save sx on stack (we clobber r6 as x counter) */
    push    {r6}
    mov     r5, r6              @ r5 = sx (saved)

.fill_x_loop:
    cmp     r5, r8
    bge     .fill_done
    mov     r6, r7              @ r6 = y = sy

.fill_y_loop:
    cmp     r6, r9
    bge     .fill_y_done

    /* offset = ((239-y) + x*240) * 3 */
    rsb     r0, r6, #239
    mov     r1, #240
    mla     r0, r5, r1, r0
    add     r1, r0, r0, lsl #1
    add     r1, r10, r1

    strb    r4, [r1, #0]        @ B
    strb    r3, [r1, #1]        @ G
    strb    r2, [r1, #2]        @ R

    add     r6, r6, #1
    b       .fill_y_loop

.fill_y_done:
    add     r5, r5, #1
    b       .fill_x_loop

.fill_done:
    pop     {r6}
    mov     r0, #0
    str     r0, [r11, #0]
    pop     {r4-r11, pc}

/* ── Literal pool ───────────────────────────────────────────────── */
    .align  2
phys_to_kva:
    .word   0x9F000000          @ 0xC0000000 - 0x21000000
l1_index_off:
    .word   0x2F84              @ 0xBE1 * 4
vram_section:
    .word   0x18301C02          @ phys 0x18300000 | SECTION_UNCACHED
fb_base:
    .word   0xBE100000

/* Mutable data embedded in code (kernel TEXT is RW on ARMv6) */
    .align  2
cached_fb_va:
    .word   0x00000000          @ Set on first call
