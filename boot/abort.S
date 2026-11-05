/*
 * abort.S — Data abort handler and exception vector table
 *
 * The HTL (Hardware Translation Layer) uses ARM data aborts to trap
 * accesses to S5L8900 peripheral addresses.  This file provides:
 *
 *   1. The data-abort assembly entry that saves state, calls the C
 *      handler (htl_handle_abort), and resumes execution.
 *
 *   2. Stub handlers for other exception types.
 *
 *   3. The exception vector table, placed in .vectors so the linker
 *      can page-align it.  The MMU maps this page to 0xFFFF0000.
 */

/* ═══════════════════════════════════════════════════════════════════════
 *  Exception handlers (.text)
 * ═══════════════════════════════════════════════════════════════════════ */

    .section .text
    .arm
    .align 4

/* ── Data abort entry ──────────────────────────────────────────────────
 *
 * On entry we are in ABT mode with IRQ/FIQ disabled.
 *   LR_abt = address of faulting instruction + 8
 *   SPSR_abt = CPSR at time of fault
 *
 * Steps:
 *   1. Save r0-r12 and LR_abt on the ABT stack.
 *   2. Read DFAR (CP15 c6) and DFSR (CP15 c5).
 *   3. Call  htl_handle_abort(dfar, dfsr, regs, fault_pc)
 *      The C handler may modify the saved register values (for loads).
 *   4. Restore r0-r12 and LR_abt.
 *   5. Return to the instruction AFTER the faulting one:
 *        SUBS PC, LR, #4   (LR_abt-4 = fault_pc+4 = next instruction)
 *      This also restores CPSR from SPSR_abt.
 */
    .global htl_data_abort_entry
htl_data_abort_entry:
    /* Save general-purpose registers and the return address */
    stmfd   sp!, {r0-r12, lr}

    /* Read fault-information coprocessor registers */
    mrc     p15, 0, r0, c6, c0, 0      @ r0 = DFAR  (faulting address)
    mrc     p15, 0, r1, c5, c0, 0      @ r1 = DFSR  (fault status)
    mov     r2, sp                      @ r2 = &saved_regs[0]  (r0-r12 on stack)
    sub     r3, lr, #8                  @ r3 = faulting instruction address

    bl      htl_handle_abort

    /* Restore registers — the C handler may have patched values for
     * emulated loads (e.g. regs[Rd] = emulated read result). */
    ldmfd   sp!, {r0-r12, lr}

    /* Return past the faulting instruction and restore CPSR */
    subs    pc, lr, #4

/* ── Undefined instruction handler ───────────────────────────────────
 *
 * ARMv7 barrier instructions (DMB, DSB, ISB, CLREX) use the 0xF prefix
 * (unconditional/NV), which is undefined on ARMv6.  XNU uses them
 * heavily.  Emulate with the ARMv6 CP15 equivalents and resume.
 *
 * On entry (UND mode):  LR_und = faulting_PC + 4
 */

    .global htl_undef_handler
htl_undef_handler:
    /* Save all GPRs so we can emulate MOVW/MOVT to any register */
    stmfd   sp!, {r0-r12, lr}
    sub     r0, lr, #4                  @ r0 = faulting instruction address
    ldr     r1, [r0]                    @ r1 = the undefined instruction

    /* ── ARMv7 barriers: 0xF57FF0xx ── */
    ldr     r2, =0xFFFFF000
    and     r3, r1, r2
    ldr     r2, =0xF57FF000
    cmp     r3, r2
    bne     .Lundef_check_clrex

    and     r3, r1, #0xF0
    cmp     r3, #0x40
    bne     1f
    mov     r2, #0
    mcr     p15, 0, r2, c7, c10, 4     @ DSB
    b       .Lundef_resume
1:  cmp     r3, #0x50
    bne     2f
    mov     r2, #0
    mcr     p15, 0, r2, c7, c10, 5     @ DMB
    b       .Lundef_resume
2:  cmp     r3, #0x60
    bne     .Lundef_unknown
    mov     r2, #0
    mcr     p15, 0, r2, c7, c5, 4      @ ISB
    b       .Lundef_resume

.Lundef_check_clrex:
    ldr     r2, =0xF57FF01F
    cmp     r1, r2
    beq     .Lundef_resume              @ CLREX: no-op on ARM11

    /* ── CP15 c9 (ARMv7 PMU) → translate to ARMv6 c15 ──
     * MCR/MRC: cond 1110 opc1 L CRn Rd 1111 opc2 1 CRm
     * Check for CRn=c9 (bits [19:16]=0x9), cp=p15 (bits [11:8]=0xF) */
    and     r2, r1, #0x0F000000
    cmp     r2, #0x0E000000             @ coprocessor instruction?
    bne     .Lundef_check_movw
    tst     r1, #0x00000010             @ bit 4 must be 1 for MCR/MRC
    beq     .Lundef_check_movw
    and     r2, r1, #0x00000F00
    cmp     r2, #0x00000F00             @ p15?
    bne     .Lundef_check_movw
    and     r2, r1, #0x000F0000
    cmp     r2, #0x00090000             @ CRn = c9?
    bne     .Lundef_check_movw

    /* It's a CP15 c9 access (ARMv7 PMU). Get Rd index. */
    and     r3, r1, #0x0000F000
    mov     r3, r3, lsr #12             @ r3 = Rd

    /* Check CRm and opc2 to determine which PMU register */
    and     r4, r1, #0x0000000F         @ CRm
    and     r5, r1, #0x000000E0
    mov     r5, r5, lsr #5              @ opc2

    /* PMCCNTR: MRC p15, 0, Rd, c9, c13, 0 → read ARM1176 CCNT */
    cmp     r4, #13                     @ CRm = c13?
    cmpeq   r5, #0                      @ opc2 = 0?
    bne     .Lundef_cp15_c9_other

    tst     r1, #0x00100000             @ L bit: 1=MRC(read), 0=MCR(write)
    beq     .Lundef_cp15_c9_other       @ writing CCNT: no-op
    mrc     p15, 0, r2, c15, c12, 1    @ Read ARM1176 CCNT
    cmp     r3, #12
    strls   r2, [sp, r3, lsl #2]        @ Store in saved regs[Rd]
    b       .Lundef_resume

.Lundef_cp15_c9_other:
    /* PMCR/PMCNTENSET/etc: no-op (bootloader already enabled counter) */
    tst     r1, #0x00100000             @ MRC (read)?
    beq     .Lundef_resume              @ MCR (write): skip
    mov     r2, #0                      @ Read: return 0
    cmp     r3, #12
    strls   r2, [sp, r3, lsl #2]
    b       .Lundef_resume

.Lundef_check_movw:
    /* ── MOVW/MOVT: cond 0011 0x00 xxxx xxxx xxxx xxxx ── */
    and     r2, r1, #0x0FB00000
    cmp     r2, #0x03000000
    bne     .Lundef_unknown             @ neither MOVW nor MOVT

    /* Extract imm16 = imm4[19:16] : imm12[11:0] */
    and     r2, r1, #0x000F0000
    mov     r2, r2, lsr #4
    ldr     r3, =0x00000FFF
    and     r3, r1, r3
    orr     r2, r2, r3                  @ r2 = imm16

    /* Rd = bits [15:12] */
    and     r3, r1, #0x0000F000
    mov     r3, r3, lsr #12

    /* Is this MOVT (bit 22 set)? */
    tst     r1, #0x00400000
    bne     .Lundef_movt

    /* MOVW: regs[Rd] = imm16 (zero-extended) */
    cmp     r3, #12
    strls   r2, [sp, r3, lsl #2]
    b       .Lundef_resume

.Lundef_movt:
    /* MOVT: regs[Rd][31:16] = imm16, preserve [15:0] */
    mov     r2, r2, lsl #16
    cmp     r3, #12
    bhi     .Lundef_resume
    ldr     r0, [sp, r3, lsl #2]
    bic     r0, r0, #0xFF000000
    bic     r0, r0, #0x00FF0000
    orr     r0, r0, r2
    str     r0, [sp, r3, lsl #2]
    b       .Lundef_resume

.Lundef_resume:
    ldmfd   sp!, {r0-r12, lr}
    movs    pc, lr                      @ return past faulting instr

.Lundef_unknown:
    bl      htl_undef_log
    ldmfd   sp!, {r0-r12, lr}
.Lundef_halt:
    mcr     p15, 0, r0, c7, c0, 4      @ WFI (ARMv6)
    b       .Lundef_halt

/* ── Supervisor call handler ──────────────────────────────────────── */

    .global htl_svc_handler
htl_svc_handler:
    stmfd   sp!, {r0-r3, lr}
    sub     r0, lr, #4                  @ r0 = SVC instruction address
    ldr     r1, [r0]                    @ r1 = instruction word
    and     r1, r1, #0x00FFFFFF         @ extract SVC number (low 24 bits)
    bl      htl_svc_log
    ldmfd   sp!, {r0-r3, lr}
    movs    pc, lr

/* ── Prefetch abort handler ───────────────────────────────────────── */

    .global htl_prefetch_abort
htl_prefetch_abort:
    stmfd   sp!, {r0-r3, lr}
    mrc     p15, 0, r0, c6, c0, 2      @ r0 = IFAR (faulting address)
    mrc     p15, 0, r1, c5, c0, 1      @ r1 = IFSR (fault status)
    sub     r2, lr, #4                  @ r2 = faulting PC
    bl      htl_prefetch_abort_log
    ldmfd   sp!, {r0-r3, lr}
    b       htl_prefetch_abort          @ hang (no recovery)

/* ── IRQ handler ──────────────────────────────────────────────────── */

    .global htl_irq_handler
htl_irq_handler:
    sub     lr, lr, #4
    stmfd   sp!, {r0-r3, lr}
    bl      htl_irq_log
    ldmfd   sp!, {r0-r3, lr}
    movs    pc, lr

/* ── FIQ handler ──────────────────────────────────────────────────── */

    .global htl_fiq_handler
htl_fiq_handler:
    b       htl_fiq_handler


/* ═══════════════════════════════════════════════════════════════════════
 *  Exception vector table (.vectors)
 *
 *  Placed in a page-aligned section in FCRAM and mapped to 0xFFFF0000
 *  by the MMU (high vectors, SCTLR.V=1).
 *
 *  Each slot is a PC-relative LDR that reads the actual handler address
 *  from the literal pool immediately below.  The literal pool values
 *  are FCRAM link-time addresses, which remain valid because FCRAM is
 *  identity-mapped.
 * ═══════════════════════════════════════════════════════════════════════ */

    .section .vectors, "ax"
    .align 5                            @ 32-byte aligned

    .global exception_vectors
exception_vectors:
    ldr     pc, vec_reset_addr          @ 0x00  Reset
    ldr     pc, vec_undef_addr          @ 0x04  Undefined Instruction
    ldr     pc, vec_svc_addr            @ 0x08  Supervisor Call
    ldr     pc, vec_pabt_addr           @ 0x0C  Prefetch Abort
    ldr     pc, vec_dabt_addr           @ 0x10  Data Abort      ← HTL
    nop                                 @ 0x14  Reserved
    ldr     pc, vec_irq_addr            @ 0x18  IRQ
    ldr     pc, vec_fiq_addr            @ 0x1C  FIQ

/* Literal pool — handler addresses resolved at link time */
vec_reset_addr: .word _start
vec_undef_addr: .word htl_undef_handler
vec_svc_addr:   .word htl_svc_handler
vec_pabt_addr:  .word htl_prefetch_abort
vec_dabt_addr:  .word htl_data_abort_entry
vec_irq_addr:   .word htl_irq_handler
vec_fiq_addr:   .word htl_fiq_handler
