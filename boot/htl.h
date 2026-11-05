/*
 * htl.h — Hardware Translation Layer for iOS 3DS bootloader
 *
 * The HTL intercepts accesses to the S5L8900 peripheral address range
 * (0x38000000-0x3FFFFFFF) via the ARM data-abort mechanism.  Those
 * addresses are left unmapped in the page tables so every load/store
 * triggers a translation fault, which our abort handler decodes and
 * dispatches to per-peripheral emulators.
 */

#ifndef HTL_H
#define HTL_H

#include "types.h"

/* S5L8900 peripheral address range trapped by the HTL */
#define S5L_IO_BASE     0x38000000
#define S5L_IO_END      0x80000000   /* kernel also maps PAs above 0x40000000 from bad DT regs */

/* ── Decoded memory access ──────────────────────────────────────────── */

typedef struct {
    u32  address;       /* Faulting virtual address (from DFAR) */
    bool is_write;      /* true = store, false = load */
    u32  reg_index;     /* ARM register (Rd), 0-12 for r0-r12 */
    u32  value;         /* For stores: value being written */
    u32  width;         /* Access width: 1 = byte, 2 = halfword, 4 = word */
} htl_access_t;

/* Initialise the HTL (informational — MMU already set up the traps). */
void htl_init(void);

/* Data-abort C handler.  Called from abort.S with:
 *   dfar     — faulting virtual address (CP15 c6)
 *   dfsr     — Data Fault Status Register (CP15 c5)
 *   regs     — pointer to saved {r0..r12} on the ABT stack
 *   fault_pc — address of the faulting instruction
 */
void htl_handle_abort(u32 dfar, u32 dfsr, u32 *regs, u32 fault_pc);

/* Decode the ARM instruction at fault_pc using the saved register
 * context.  Fills access->is_write, reg_index, value, width.
 * Returns true if the instruction was recognised. */
bool htl_decode_instruction(u32 instr, u32 *regs, htl_access_t *access);

/* Dispatch an emulated access.  Returns the value for loads. */
u32  htl_dispatch(htl_access_t *access);

/* ── Exception logging (called from abort.S) ──────────────────────── */

/* Log a prefetch abort: faulting address (IFAR), fault status (IFSR), PC */
void htl_prefetch_abort_log(u32 ifar, u32 ifsr, u32 pc);

/* Log an undefined instruction: faulting PC and the instruction word */
void htl_undef_log(u32 pc, u32 instr);

/* Log a supervisor call: faulting PC and SVC number (first 10 only) */
void htl_svc_log(u32 pc, u32 svc_num);

/* Log an IRQ (first 5 only) */
void htl_irq_log(void);

#endif /* HTL_H */
