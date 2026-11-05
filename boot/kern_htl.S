/*
 * kern_htl.S — Exception handlers that survive kernel vector installation
 *
 * Part of the payload image, which is linked in the kernel's linear map
 * (VA 0xC08xxxxx, see iowin.h).  The kernel's exception vector template is
 * patched so that its data abort, prefetch abort, undefined instruction and
 * IRQ entries point here.
 *
 * Data abort handler:
 *   Installs our device window into the kernel's TTBR1 table (once), then
 *   emulates S5L8900 peripheral accesses in C; every other abort re-enters
 *   the kernel's own data abort handler.
 *
 * Undefined instruction handler:
 *   Logs, then chains everything (VFP lazy-enable traps included) to the
 *   kernel's original undefined instruction handler.
 */

/* The kernel's shared exception return (0xC00603E4) leaves SP_abt pointing
 * into a thread's kernel stack, so never trust the banked SP: switch to our
 * own stack unless this exception nested inside our handler of the same
 * mode.  Must run before anything is pushed.  Clobbers only flags. */
    .macro OWN_STACK mode, top
    str     r0, kern_exc_scratch
    mrs     r0, spsr
    and     r0, r0, #0x1F
    cmp     r0, #\mode
    ldrne   sp, =\top
    ldr     r0, kern_exc_scratch
    .endm

    .section .kern_htl, "ax"
    .arm
    .align 4

/* ═══════════════════════════════════════════════════════════════════
 *  Data Abort Handler
 * ═══════════════════════════════════════════════════════════════════ */

    .global kern_htl_da_entry
kern_htl_da_entry:
    OWN_STACK 0x17, kern_htl_abt_stack_top
    stmfd   sp!, {r0-r12, lr}
    /* Aborts from FIQ mode: the frame must hold the FIQ-banked r8-r12,
     * not the usr/sys bank this mode sees.  Park the usr bank and swap. */
    mrs     r0, spsr
    and     r0, r0, #0x1F
    cmp     r0, #0x11
    bne     1f
    add     r0, sp, #32                 @ &frame r8
    ldmia   r0, {r1-r5}
    ldr     r6, =kern_usr_r8_save
    stmia   r6, {r1-r5}
    mrs     r1, cpsr
    msr     cpsr_c, #0xD1               @ FIQ mode, I+F masked
    stmia   r0, {r8-r12}
    msr     cpsr_c, r1
1:

    bl      kern_map_window
    ldr     lr, [sp, #52]               @ restore LR_abt clobbered by bl

    /* All decisions in C: translate DFAR to PA, emulate S5L8900
     * peripheral accesses, log and skip anything else. */
    mrc     p15, 0, r0, c6, c0, 0      @ DFAR
    mrc     p15, 0, r1, c5, c0, 0      @ DFSR
    mov     r2, sp                      @ saved r0-r12, lr
    sub     r3, lr, #8                  @ fault PC
    ldr     r5, =htl_kern_abort
    blx     r5                          @ r0 = resume address, 0 = chain
    cmp     r0, #0
    beq     3f

    str     r0, [sp, #52]
    mrs     r0, spsr
    and     r0, r0, #0x1F
    cmp     r0, #0x11
    bne     2f
    add     r0, sp, #32
    mrs     r1, cpsr
    msr     cpsr_c, #0xD1
    ldmia   r0, {r8-r12}                @ emulated results -> FIQ bank
    msr     cpsr_c, r1
    ldr     r6, =kern_usr_r8_save
    ldmia   r6, {r1-r5}
    stmia   r0, {r1-r5}                 @ usr bank restored unchanged
2:
    ldmfd   sp!, {r0-r12, lr}
    movs    pc, lr                      @ SPSR -> CPSR keeps the T bit

3:  /* Not a peripheral access: re-enter the kernel's data abort handler
     * exactly as the hardware would (original regs, LR_abt, SPSR, DFSR,
     * DFAR; _fleh_dataabt only uses SP_abt as scratch). */
    ldmfd   sp!, {r0-r12, lr}
    ldr     pc, kern_da_orig_addr

    .align 2
kern_usr_r8_save:
    .space  20
kern_exc_scratch:
    .word   0

/* Install our device window (iowin.h) into the kernel's TTBR1 table, once.
 * TTBR1 is the kernel's own table and is shared by every address space, so
 * user TTBR0 tables are never touched.  Our code and data already live in
 * the kernel's linear map (0xC08xxxxx).  The table is reached through that
 * map (phys + 0x9F000000).  Entries are privileged-only (AP=01), global:
 *   0x800-0x80F  PA 0x10000000+   device
 *   0x810        PA 0x17E00000    device (MPCore private: GIC, timers)
 *   0x818-0x81F  PA 0x18000000+   normal non-cacheable (VRAM)
 *   0x820-0x82F  PA 0x20000000+   normal non-cacheable, 0x828 (iPhone FB) WB
 * Clobbers r0, r4-r9. */
/* Keep our device window (iowin.h) present in the kernel's TTBR1 table.
 * TTBR1 is the kernel's own table, shared by every address space.  Every
 * call checks all 41 window entries and rewrites the ones that differ; the
 * first mismatch per group is kept in kern_win_rec for htl.c to report.  Entries are
 * privileged-only (AP=01), global:
 *   0x800-0x80F  PA 0x10000000+   device
 *   0x810        PA 0x17E00000    device (MPCore private: GIC, timers)
 *   0x818-0x81F  PA 0x18000000+   normal non-cacheable (VRAM)
 *   0x820-0x82F  PA 0x20000000+   normal non-cacheable, 0x828 (iPhone FB) WB
 * kern_map_window_force rewrites unconditionally.  Clobbers r0-r9, ip, lr. */
    .global kern_map_window_force
kern_map_window_force:
    mov     r0, #1
    b       .Lkmw_start
kern_map_window:
    mov     r0, #0
.Lkmw_start:
    mov     r4, lr                      @ inner calls use lr
    mrc     p15, 0, r5, c2, c0, 1      @ TTBR1
    ldr     r6, =0x3FFF
    bic     r9, r5, r6                 @ r9 = table phys (16KB aligned)
    sub     r6, r9, #0x21000000
    cmp     r6, #0x07000000            @ inside the kernel's linear map?
    bhs     .Lkmw_done
    add     r8, r9, #0x9F000000        @ r8 = table kernel VA
    add     r7, r8, #0x2000            @ r7 = &L1[0x800]
    mov     r9, #0                      @ r9 = "something written"

    ldr     r6, =0x10000416            @ 0x800-0x80F
    mov     r5, #16
.Lkmw_io:
    bl      .Lkmw_put
    add     r6, r6, #0x00100000
    subs    r5, r5, #1
    bne     .Lkmw_io
    ldr     r6, =0x17E00416            @ 0x810
    bl      .Lkmw_put
    add     r7, r7, #28                @ skip 0x811-0x817
    ldr     r6, =0x18001412            @ 0x818-0x81F
    mov     r5, #8
.Lkmw_vram:
    bl      .Lkmw_put
    add     r6, r6, #0x00100000
    subs    r5, r5, #1
    bne     .Lkmw_vram
    ldr     r6, =0x20001412            @ 0x820-0x82F, 0x828 cacheable
    mov     r5, #16
.Lkmw_low:
    cmp     r5, #8
    orreq   r6, r6, #0x0C
    bl      .Lkmw_put
    bic     r6, r6, #0x0C
    add     r6, r6, #0x00100000
    subs    r5, r5, #1
    bne     .Lkmw_low

    cmp     r9, #0
    beq     .Lkmw_done
    mov     r0, #1
    str     r0, kern_win_installed
    mov     r0, #0
    mcr     p15, 0, r0, c7, c10, 0     @ clean D-cache (table walker reads RAM)
    mcr     p15, 0, r0, c7, c10, 4     @ DSB
    mcr     p15, 0, r0, c8, c7, 0      @ TLBIALL
    mcr     p15, 0, r0, c7, c10, 4     @ DSB
    mcr     p15, 0, r0, c7, c5, 4      @ ISB
.Lkmw_done:
    bx      r4

/* Store r6 at [r7] (post-increment) if it differs or r0 != 0.  After the
 * first install, the first mismatch in each window group is recorded in
 * kern_win_rec[group] = {L1 index, value found, TTBR1, SPSR}.  Scratch:
 * r1-r3, ip (saved by every caller's frame). */
.Lkmw_put:
    ldr     r1, [r7]
    cmp     r1, r6
    cmpeq   r0, #0
    beq     1f
    ldr     r2, kern_win_installed
    cmp     r2, #0                      @ first install isn't evidence
    beq     2f
    cmp     r0, #0                      @ forced rewrites aren't evidence
    bne     2f
    sub     r2, r7, r8
    mov     r2, r2, lsr #2              @ L1 index
    sub     r3, r2, #0x800
    cmp     r3, #0x10
    movlo   r3, #0                      @ group 0: IO
    blo     3f
    cmp     r3, #0x11
    movlo   r3, #1                      @ group 1: MPCore
    blo     3f
    cmp     r3, #0x20
    movlo   r3, #2                      @ group 2: VRAM
    movhs   r3, #3                      @ group 3: low FCRAM
3:  adr     ip, kern_win_rec
    add     ip, ip, r3, lsl #4
    ldr     r3, [ip]
    cmp     r3, #0
    bne     2f
    str     r2, [ip]
    str     r1, [ip, #4]
    mrc     p15, 0, r3, c2, c0, 1
    str     r3, [ip, #8]
    mrs     r3, spsr
    str     r3, [ip, #12]
2:  str     r6, [r7]
    mov     r9, #1
1:  add     r7, r7, #4
    mov     pc, lr

    .align 2
    .global kern_win_rec
kern_win_rec:
    .space  64                          @ 4 groups x {idx, val, ttbr1, spsr}
kern_win_installed:
    .word   0

    .ltorg

    .align 2
    .global kern_da_orig_addr
kern_da_orig_addr:
    .word 0                             @ patched at runtime




/* ═══════════════════════════════════════════════════════════════════
 *  Undefined Instruction Handler
 *
 *  Log, then hand every undefined instruction to the kernel's own handler
 *  untouched.  That includes VFP (cp10/cp11): the kernel enables VFP
 *  lazily, taking an undef with FPEXC.EN clear and switching the thread's
 *  VFP state in _sleh_undef -> _vfp_trap.
 *
 *  On entry (UND mode): LR_und = faulting PC + 4 (ARM) or + 2 (Thumb).
 * ═══════════════════════════════════════════════════════════════════ */

    .global kern_htl_undef_entry
kern_htl_undef_entry:
    OWN_STACK 0x1B, kern_htl_und_stack_top
    stmfd   sp!, {r0-r12, lr}
    bl      kern_map_window
    /* Our own trap points (pm.c: UDF patched into the kernel's sleep
     * path) are handled here: r0 = new SPSR, frame updated, 0 = not ours. */
    mov     r0, sp
    mrs     r1, spsr
    ldr     r5, =htl_kern_undef_hook
    blx     r5
    cmp     r0, #0
    beq     1f
    msr     spsr_cxsf, r0
    ldmfd   sp!, {r0-r12, lr}
    movs    pc, lr                      @ resume at frame LR, SPSR -> CPSR
1:
    ldr     r0, [sp, #52]               @ LR_und
    mrs     r1, spsr
    ldr     r5, =htl_kern_undef_log
    blx     r5
    ldmfd   sp!, {r0-r12, lr}
    ldr     pc, kern_undef_orig_addr

    .align 2
    .global kern_undef_orig_addr
kern_undef_orig_addr:
    .word 0                             @ patched at runtime



    .global kern_htl_pabt_entry
kern_htl_pabt_entry:
    OWN_STACK 0x17, kern_htl_abt_stack_top
    stmfd   sp!, {r0-r12, lr}
    bl      kern_map_window
    ldr     r0, [sp, #52]               @ LR_abt = faulting PC + 4
    mrc     p15, 0, r1, c5, c0, 1       @ IFSR
    mrs     r2, spsr
    ldr     r5, =htl_kern_pabt_log
    blx     r5
    ldr     lr, [sp, #52]
    ldmfd   sp!, {r0-r12, lr}
    ldr     pc, kern_pabt_orig_addr

    .align 2
    .global kern_pabt_orig_addr
kern_pabt_orig_addr:
    .word 0                             @ patched at runtime

    .ltorg

/* ═══════════════════════════════════════════════════════════════════
 *  IRQ -> FIQ bridge
 *
 *  The MPCore GIC can only raise IRQ, but the kernel takes its timer tick
 *  on FIQ (VIC0 line 7, FIQ vector = `mov pc, r9` with FIQ-banked r9).
 *  The kernel's own IRQ path is replaced, so IRQ mode and its SP are ours.
 *  htl_kern_irq() acks the GIC/timer and returns non-zero when an FIQ
 *  should be delivered; we then enter FIQ mode exactly as the hardware
 *  would (SPSR_fiq = interrupted CPSR, LR_fiq = return address + 4).
 * ═══════════════════════════════════════════════════════════════════ */

    .global kern_htl_irq_entry
kern_htl_irq_entry:
    ldr     sp, =kern_htl_irq_stack_top
    stmfd   sp!, {r0-r12, lr}
    bl      kern_map_window
    mrs     r0, spsr
    ldr     r1, [sp, #52]               @ LR_irq: interrupted PC + 4
    ldr     r5, =htl_kern_irq
    blx     r5
    cmp     r0, #1                      @ 0 none, 1 FIQ, 2 VIC IRQ
    ldmfd   sp!, {r0-r12, lr}           @ (flags survive the reload)
    sublos  pc, lr, #4                  @ nothing to deliver
    ldrhi   pc, kern_irq_orig_addr      @ _fleh_irq: IRQ mode, LR/SPSR as
                                        @ the hardware left them

    stmfd   sp!, {r0, r1}
    mrs     r0, spsr
    mov     r1, lr
    msr     cpsr_c, #0xD1               @ FIQ mode, I+F masked, ARM
    msr     spsr_cxsf, r0
    mov     lr, r1
    msr     cpsr_c, #0xD2               @ back to IRQ mode to restore r0/r1
    ldmfd   sp!, {r0, r1}
    msr     cpsr_c, #0xD1
    str     r9, kern_fiq_target         @ record for the prefetch-abort log
    mov     pc, r9                      @ kernel FIQ entry (FIQ-banked r9)

    .align 2
    .global kern_fiq_target
kern_fiq_target:
    .word 0
    .global kern_irq_orig_addr
kern_irq_orig_addr:
    .word 0

    .ltorg

/* Mach trap -10 (framebuffer), installed by the loader in place of the
 * kernel's cave.  SVC mode, process context: with IRQ/FIQ masked for the
 * whole (short) call, check the device window, then let display.c draw
 * into the iPhone framebuffer.  r0 = args; the result goes to r0 and
 * args[0]. */
    .global kern_htl_fb_trap
kern_htl_fb_trap:
    push    {r4-r12, lr}
    mov     r10, r0
    mrs     r11, cpsr
    cpsid   if                          @ whole call: short, and our IRQ
    bl      kern_map_window             @ bridge must not enter the kernel's
    mov     r0, r10                     @ FIQ/IRQ handlers on top of it
    ldr     r1, =htl_fb_trap
    blx     r1
    str     r0, [r10]
    msr     cpsr_c, r11
    pop     {r4-r12, pc}

    .ltorg

/* ═══════════════════════════════════════════════════════════════════
 *  Stacks for ABT and UND modes (at kernel VA, always mapped)
 * ═══════════════════════════════════════════════════════════════════ */

    .section .kern_htl.data, "aw"
    .align 12                           @ 4KB-aligned

    .space 4096
    .global kern_htl_abt_stack_top
kern_htl_abt_stack_top:

    .space 4096
    .global kern_htl_und_stack_top
kern_htl_und_stack_top:

    .space 4096
    .global kern_htl_irq_stack_top
kern_htl_irq_stack_top:
