/*
 * htl.c — Hardware Translation Layer for iOS 3DS bootloader
 *
 * Handles data aborts caused by the iPhone OS kernel accessing S5L8900
 * peripheral registers.  Decodes the faulting ARM instruction, figures
 * out what the kernel wanted, and returns an emulated value (loads) or
 * performs the emulated side-effect (stores).
 *
 * For now the peripheral emulators are stubs that return zero / ignore
 * writes and log the first few accesses for debugging.
 */

#include "htl.h"
#include "iowin.h"
#include "periph/vic.h"
#include "display.h"
#include "periph/uart.h"
#include "periph/input.h"
#include "console.h"
#include "periph/periph.h"
#include "periph/timer.h"
#include "periph/pl080.h"

/* ── Access log ─────────────────────────────────────────────────────── */

static u32 htl_log_max;   /* raise to trace peripheral accesses */
static u32 htl_log_count;

/* ── Instruction decoder ────────────────────────────────────────────── */

/*
 * ARM LDR/STR (word/byte, immediate or register offset)
 *   cond[31:28] 01 I P U B W L Rn[19:16] Rd[15:12] offset[11:0]
 *     bits [27:26] = 01
 *     L (20): 1 = load, 0 = store
 *     B (22): 1 = byte, 0 = word
 *
 * ARM LDRH/STRH / LDRSB/LDRSH (halfword / signed)
 *   cond[31:28] 000 P U I W L Rn[19:16] Rd[15:12] ... 1 S H 1 ...
 *     bits [27:25] = 000,  bit [7] = 1,  bit [4] = 1
 *     SH [6:5]: 01 = unsigned halfword, 10 = signed byte, 11 = signed halfword
 *     L (20): 1 = load, 0 = store
 */

bool htl_decode_instruction(u32 instr, u32 *regs, htl_access_t *access) {
    u32 rd;
    bool is_load;

    /* ── Case 1: word / byte LDR / STR ─────────────────────────────── */
    if ((instr & 0x0C000000) == 0x04000000) {
        rd      = (instr >> 12) & 0xF;
        is_load = (instr >> 20) & 1;

        access->is_write  = !is_load;
        access->reg_index = rd;
        access->width     = ((instr >> 22) & 1) ? 1 : 4;  /* B bit */

        if (access->is_write && rd <= 12)
            access->value = regs[rd];
        else if (access->is_write)
            access->value = 0;  /* SP/LR/PC — unusual for MMIO */

        return true;
    }

    /* ── Case 2: halfword / signed-byte LDR / STR ──────────────────── */
    if ((instr & 0x0E000090) == 0x00000090) {
        u32 sh = (instr >> 5) & 3;
        if (sh == 0)
            return false;   /* SH=00 → multiply/swap, not load/store */

        rd      = (instr >> 12) & 0xF;
        is_load = (instr >> 20) & 1;

        access->is_write  = !is_load;
        access->reg_index = rd;
        access->width     = (sh == 2) ? 1 : 2;  /* SH=10 signed byte, else halfword */

        if (access->is_write && rd <= 12)
            access->value = regs[rd];
        else if (access->is_write)
            access->value = 0;

        return true;
    }

    /* Unrecognised (could be LDM/STM or coprocessor — add later) */
    return false;
}

/* ── Peripheral dispatch (stubs) ────────────────────────────────────── */

u32 htl_dispatch(htl_access_t *access) {
    /* Log the first N accesses to the debug console */
    if (htl_log_count < htl_log_max) {
        console_print("[HTL] ");
        console_print(access->is_write ? "WR" : "RD");

        switch (access->width) {
        case 1: console_print("8  "); break;
        case 2: console_print("16 "); break;
        default: console_print("32 "); break;
        }

        console_print_hex(access->address);

        if (access->is_write) {
            console_print(" <- ");
            console_print_hex(access->value);
        }
        console_print("\n");

        htl_log_count++;
        if (htl_log_count == htl_log_max)
            console_print("[HTL] (further accesses suppressed)\n");
    }

    CRUMB_IN(1);
    u32 r = periph_dispatch(access->address, access->value,
                            access->is_write, access->width);
    CRUMB_OUT();
    return r;
}

/* ── Exception logging (called from abort.S) ──────────────────────── */

void htl_prefetch_abort_log(u32 ifar, u32 ifsr, u32 pc) {
    console_print("\n[HTL] PREFETCH ABORT at ");
    console_print_hex(pc);
    console_print(" addr=");
    console_print_hex(ifar);
    console_print(" ifsr=");
    console_print_hex(ifsr);
    console_print("\n");
}

void htl_undef_log(u32 pc, u32 instr) {
    console_print("\n[HTL] UNDEF at ");
    console_print_hex(pc);
    console_print(" instr=");
    console_print_hex(instr);
    console_print("\n");
    /* Also print the 4 instructions around the fault for context */
    u32 *p = (u32 *)(pc & ~3u);
    for (int i = -1; i <= 2; i++) {
        console_print("  ");
        console_print_hex((u32)(p + i));
        console_print(": ");
        console_print_hex(p[i]);
        if (i == 0) console_print(" <-- FAULT");
        console_print("\n");
    }
}

/* Only log the first N SVCs to avoid flooding the console */
static u32 svc_count;

void htl_svc_log(u32 pc, u32 svc_num) {
    if (svc_count < 10) {
        console_print("[HTL] SVC #");
        console_print_hex(svc_num);
        console_print(" at ");
        console_print_hex(pc);
        console_print("\n");
        svc_count++;
        if (svc_count == 10)
            console_print("[HTL] (further SVCs suppressed)\n");
    }
}

void htl_irq_log(void) {
    /* Minimal — IRQs will be frequent once enabled */
    static u32 irq_count;
    if (irq_count < 5) {
        console_print("[HTL] IRQ\n");
        irq_count++;
        if (irq_count == 5)
            console_print("[HTL] (further IRQs suppressed)\n");
    }
}

/* ── Register dump helper ──────────────────────────────────────────── */

static void htl_dump_regs(u32 *regs, u32 fault_pc) {
    static const char *rnames[] = {
        "r0=", "r1=", "r2=", "r3=",
        "r4=", "r5=", "r6=", "r7=",
        "r8=", "r9=", "r10=", "r11=",
        "r12="
    };
    for (u32 i = 0; i < 13; i++) {
        if ((i & 3) == 0 && i > 0)
            console_print("\n");
        console_print("  ");
        console_print(rnames[i]);
        console_print_hex(regs[i]);
    }
    console_print("\n  PC=");
    console_print_hex(fault_pc);
    console_print("\n");
}

/* ── Data-abort C handler ───────────────────────────────────────────── */

void htl_handle_abort(u32 dfar, u32 dfsr, u32 *regs, u32 fault_pc) {
    /* DFSR fault status: bits [3:0] + bit [10] → FS[4:0]
     *   Section translation fault = 5
     *   Page translation fault    = 7  */
    u32 fs = (dfsr & 0xF) | ((dfsr >> 6) & 0x10);

    /* Is this a translation fault? */
    if (fs != 5 && fs != 7) {
        console_print("\n[HTL] FATAL: unexpected abort, FS=");
        console_print_hex(fs);
        console_print(" DFAR=");
        console_print_hex(dfar);
        console_print(" DFSR=");
        console_print_hex(dfsr);
        console_print(" PC=");
        console_print_hex(fault_pc);
        console_print("\n  instr=");
        console_print_hex(*(u32 *)fault_pc);
        console_print("\n");
        htl_dump_regs(regs, fault_pc);
        for (;;) __asm__ volatile("wfi");
    }

    /* Is the faulting address in the S5L8900 IO range? */
    if (dfar < S5L_IO_BASE || dfar >= S5L_IO_END) {
        static u32 non_s5l_count;
        if (non_s5l_count < 16) {
            console_print("[HTL] WARN: non-S5L fault at ");
            console_print_hex(dfar);
            console_print(" PC=");
            console_print_hex(fault_pc);
            console_print("\n");
            non_s5l_count++;
            if (non_s5l_count == 16)
                console_print("[HTL] (further non-S5L faults suppressed)\n");
        }
        /* Emulate as returning 0 for loads, ignore writes */
        u32 instr = *(u32 *)fault_pc;
        htl_access_t access;
        access.address = dfar;
        if (htl_decode_instruction(instr, regs, &access)) {
            if (!access.is_write && access.reg_index <= 12)
                regs[access.reg_index] = 0;
            return;
        }
        /* Can't decode — halt */
        console_print("\n[HTL] FATAL: cannot decode non-S5L fault ");
        console_print_hex(*(u32 *)fault_pc);
        console_print(" at PC=");
        console_print_hex(fault_pc);
        console_print("\n");
        htl_dump_regs(regs, fault_pc);
        for (;;) __asm__ volatile("wfi");
    }

    /* Decode the faulting instruction */
    u32 instr = *(u32 *)fault_pc;
    htl_access_t access;
    access.address = dfar;

    if (!htl_decode_instruction(instr, regs, &access)) {
        console_print("\n[HTL] FATAL: cannot decode ");
        console_print_hex(instr);
        console_print(" at PC=");
        console_print_hex(fault_pc);
        console_print("\n");
        htl_dump_regs(regs, fault_pc);
        for (;;) __asm__ volatile("wfi");
    }

    /* Dispatch to peripheral emulator */
    u32 result = htl_dispatch(&access);

    /* For loads: write the emulated value back into the saved register
     * so it is restored when the abort handler returns. */
    if (!access.is_write && access.reg_index <= 12) {
        regs[access.reg_index] = result;
    }
}

/* ── HTL init ───────────────────────────────────────────────────────── */

void htl_init(void) {
    htl_log_count = 0;

    console_print("[HTL] Data abort handler installed\n");
    console_print("[HTL] Trap range: 0x38000000-0x3FFFFFFF\n");

    /* Verify high vectors are active */
    u32 sctlr;
    __asm__ volatile("mrc p15, 0, %0, c1, c0, 0" : "=r"(sctlr));
    if (sctlr & (1 << 13))
        console_print("[HTL] High vectors active (V=1)\n");
    else
        console_print("[HTL] WARNING: high vectors NOT active!\n");

    console_print("[HTL] Ready\n");
}

/* ── Data aborts taken under the kernel's page tables ─────────────────
 *
 * The kernel maps peripherals itself (e.g. PA 0x3Cxxxxxx at a VA it picks,
 * such as 0xE0000000), so the S5L8900 range must be matched on the
 * physical address.  On the 3DS nothing decodes those PAs and the access
 * raises a precise external abort; translation faults in VA 0x38-0x3F
 * (identity view) are matched directly.
 */

#define TTBR_BASE(t) ((t) & 0xFFFFC000u)

/* Page tables are kernel-allocated: reach them through the kernel's
 * linear window (phys 0x21000000+ -> VA 0xC0000000+, 112MB on O3DS), not
 * identity, which only exists for the few sections kern_map_fcram adds.
 * Returns 0 if the address is outside that window. */
static u32 kern_table_va(u32 pa) {
    if (pa - 0x21000000u < 0x07000000u)
        return pa + 0x9F000000u;
    return 0;
}

static void kern_console(void);

/* Per-MB 32-bit word sums of the 4 MB ramdisk at base (debug). */
void rd_print_sums(u32 base) {
    console_print("RD");
    for (u32 mb = 0; mb < 4; mb++) {
        const volatile u32 *p = (const volatile u32 *)(base + mb * 0x100000);
        u32 sum = 0;
        for (u32 i = 0; i < 0x40000; i++)
            sum += p[i];
        console_print(" ");
        console_print_hex(sum);
    }
    console_print("\n");
}

/* Walk the live tables for va (TTBR0/TTBR1 per TTBCR.N).
 * Returns 0xFFFFFFFF if unmapped. */
static u32 kern_va_to_pa(u32 va) {
    u32 ttbcr, ttbr, l1_base;
    __asm__ volatile("mrc p15, 0, %0, c2, c0, 2" : "=r"(ttbcr));
    u32 n = ttbcr & 7;
    if (n && (va >> (32 - n)) == 0) {
        __asm__ volatile("mrc p15, 0, %0, c2, c0, 0" : "=r"(ttbr));
        l1_base = ttbr & ~((1u << (14 - n)) - 1);
    } else if (n) {
        __asm__ volatile("mrc p15, 0, %0, c2, c0, 1" : "=r"(ttbr));
        l1_base = TTBR_BASE(ttbr);
    } else {
        __asm__ volatile("mrc p15, 0, %0, c2, c0, 0" : "=r"(ttbr));
        l1_base = TTBR_BASE(ttbr);
    }

    u32 l1p = kern_table_va(l1_base + ((va >> 20) << 2));
    if (!l1p)
        return 0xFFFFFFFFu;
    u32 l1 = *(volatile u32 *)l1p;
    switch (l1 & 3) {
    case 2:
        if (l1 & (1u << 18))
            return (l1 & 0xFF000000u) | (va & 0x00FFFFFFu);
        return (l1 & 0xFFF00000u) | (va & 0x000FFFFFu);
    case 1: {
        u32 l2p = kern_table_va((l1 & 0xFFFFFC00u) + (((va >> 12) & 0xFF) << 2));
        if (!l2p)
            return 0xFFFFFFFFu;
        u32 l2 = *(volatile u32 *)l2p;
        if ((l2 & 3) == 1)
            return (l2 & 0xFFFF0000u) | (va & 0xFFFFu);
        if (l2 & 2)
            return (l2 & 0xFFFFF000u) | (va & 0xFFFu);
        return 0xFFFFFFFFu;
    }
    default:
        return 0xFFFFFFFFu;
    }
}

u32 htl_kva_pa(u32 va) {
    return kern_va_to_pa(va);
}

/* Fault-free reads for exception paths: translate through the live tables
 * (TTBR0 for user VAs) and read through the kernel's linear FCRAM window.
 * Anything unmapped or outside FCRAM reads as "absent" instead of taking an
 * abort inside our handler. */
static bool safe_rd32(u32 va, u32 *out) {
    u32 pa = kern_va_to_pa(va & ~3u);
    u32 lin = pa == 0xFFFFFFFFu ? 0 : kern_table_va(pa);
    if (!lin) {
        *out = 0xDEADDEAD;
        return false;
    }
    *out = *(volatile u32 *)lin;
    return true;
}

/* The instruction at pc (Thumb: the halfword). */
static bool safe_insn(u32 pc, bool thumb, u32 *out) {
    u32 w;
    bool ok = safe_rd32(pc, &w);
    *out = !thumb ? w : (pc & 2) ? w >> 16 : w & 0xFFFF;
    return ok;
}

static u32 safe_word(u32 va) {
    u32 w;
    return safe_rd32(va, &w) ? w : 0;
}

/* Apply the base-register update of a skipped LDR/STR/LDRH/STRH
 * (post-indexed or pre-indexed with writeback).  Rn must be r0-r12. */
static void kern_apply_writeback(u32 instr, u32 *regs) {
    u32 p = (instr >> 24) & 1, u = (instr >> 23) & 1, w = (instr >> 21) & 1;
    u32 rn = (instr >> 16) & 0xF, off;
    if (!(p == 0 || w) || rn > 12)
        return;

    if ((instr & 0x0C000000) == 0x04000000) {
        if (!(instr & (1u << 25))) {
            off = instr & 0xFFF;
        } else {
            u32 rm = instr & 0xF, sh = (instr >> 7) & 0x1F, type = (instr >> 5) & 3;
            if (rm > 12 || type != 0)
                return;
            off = regs[rm] << sh;
        }
    } else {
        if (instr & (1u << 22)) {
            off = ((instr >> 4) & 0xF0) | (instr & 0xF);
        } else {
            u32 rm = instr & 0xF;
            if (rm > 12)
                return;
            off = regs[rm];
        }
    }
    regs[rn] = u ? regs[rn] + off : regs[rn] - off;
}

/* Decode a 16-bit Thumb load/store (ARMv6 Thumb-1). No writeback forms. */
static bool kern_decode_thumb(u16 t, u32 *regs, htl_access_t *a) {
    u32 rd = t & 7, l, w;
    if ((t & 0xF000) == 0x5000) {               /* register offset */
        static const u8 width[8]  = { 4, 2, 1, 1, 4, 2, 1, 2 };
        static const u8 isload[8] = { 0, 0, 0, 1, 1, 1, 1, 1 };
        u32 op = (t >> 9) & 7;
        l = isload[op]; w = width[op];
    } else if ((t & 0xE000) == 0x6000) {        /* LDR/STR(B) imm5 */
        l = (t >> 11) & 1; w = (t & 0x1000) ? 1 : 4;
    } else if ((t & 0xF000) == 0x8000) {        /* LDRH/STRH imm5 */
        l = (t >> 11) & 1; w = 2;
    } else if ((t & 0xF000) == 0x9000) {        /* SP-relative */
        l = (t >> 11) & 1; w = 4; rd = (t >> 8) & 7;
    } else {
        return false;
    }
    a->is_write  = !l;
    a->reg_index = rd;
    a->width     = w;
    if (a->is_write)
        a->value = regs[rd];
    return true;
}

/* Switch the console to wrap mode the first time the kernel traps. */
static void kern_console(void) {
    static bool done;
    if (!done) {
        console_home();
        done = true;
    }
}

/* Log undefined-instruction exceptions taken under the kernel. */
u32 kern_und_count;          /* undefined-instruction traps seen */
static bool und_check_pending;

/* After an undef went to the kernel, sample VFP state at the next
 * exception of any kind: FPEXC, and the kernel's VFP owner (cpu data
 * +0xD4) against the current thread's VFP slot (thread +0x290).  The
 * current thread comes from the variable _fleh_undef loads r9 from. */
static void und_followup(const char *where, u32 spsr, u32 pc) {
    static u32 n;
    if (!und_check_pending)
        return;
    und_check_pending = false;
    if (n++ >= 6)
        return;
    u32 fpexc;
    __asm__ volatile("mrc p10, 7, %0, c8, c0, 0" : "=r"(fpexc));
    u32 cur_var = safe_word(0xC0060720);
    u32 thread = cur_var ? safe_word(cur_var) : 0;
    u32 cpu = thread ? safe_word(thread + 0x2A0) : 0;
    u32 owner = cpu ? safe_word(cpu + 0xD4) : 0;
    kern_console();
    console_print("[VFP] next ");
    console_print(where);
    console_print(" FPEXC=");
    console_print_hex(fpexc);
    console_print(" m=");
    console_print_hex(spsr & 0x1F);
    console_print(" PC=");
    console_print_hex(pc);
    console_print("\n thr=");
    console_print_hex(thread);
    console_print(" own=");
    console_print_hex(owner);
    console_print(" slot=");
    console_print_hex(thread + 0x290);
    console_print("\n");
}

void htl_kern_undef_log(u32 lr_und, u32 spsr) {
    static u32 n;
    kern_und_count++;
    und_check_pending = true;
    if (n++ >= 10)
        return;
    bool thumb = (spsr >> 5) & 1;
    u32 pc = lr_und - (thumb ? 2 : 4);
    u32 fpexc, fpsid, cpacr;
    __asm__ volatile("mrc p15, 0, %0, c1, c0, 2" : "=r"(cpacr));
    __asm__ volatile("mrc p10, 7, %0, c8, c0, 0" : "=r"(fpexc));  /* FPEXC */
    __asm__ volatile("mrc p10, 7, %0, c0, c0, 0" : "=r"(fpsid));  /* FPSID */
    kern_console();
    console_print("UND PC=");
    console_print_hex(pc);
    console_print(" I=");
    u32 insn;
    safe_insn(pc, thumb, &insn);
    console_print_hex(insn);
    console_print(thumb ? " T m=" : " A m=");
    console_print_hex(spsr & 0x1F);
    console_print("\n FPEXC=");
    console_print_hex(fpexc);
    console_print(" CPACR=");
    console_print_hex(cpacr);
    console_print(" FPSID=");
    console_print_hex(fpsid);
    console_print("\n");
}

/* ── Live data-abort statistics for the pinned status line ─────────── */

static u32 kda_total, kda_last_pa, kda_last_pc, kda_last_val;
static u32 kern_fiq_delivered;  /* timer FIQs handed to the kernel */
static bool kda_last_write;

#define KDA_TOP_SLOTS 32
static struct { u32 pa, count; } kda_top[KDA_TOP_SLOTS];

/* Space-saving heavy-hitter count of peripheral PAs. */
static void kda_count_pa(u32 pa) {
    u32 min = 0;
    for (u32 i = 0; i < KDA_TOP_SLOTS; i++) {
        if (kda_top[i].pa == pa && kda_top[i].count) {
            kda_top[i].count++;
            return;
        }
        if (kda_top[i].count < kda_top[min].count)
            min = i;
    }
    kda_top[min].pa = pa;
    kda_top[min].count++;
}

static char *put_hex(char *p, u32 v, u32 digits) {
    static const char hex[] = "0123456789ABCDEF";
    for (int i = (int)digits - 1; i >= 0; i--)
        *p++ = hex[(v >> (i * 4)) & 0xF];
    return p;
}

/* Kernel-mode aborts handed to the kernel's handler (registers only, no
 * memory read at the fault): the last one, and the last "interesting" one
 * (not a routine user-page fault, or an address under 64 KB: NULL-ish),
 * kept for the pinned status rows so a panic's cause survives its text. */
static struct { u32 n, dfar, dfsr, pc, spsr; } kf_last, kf_bad;

static void kfault_note(u32 dfar, u32 dfsr, u32 pc, u32 spsr) {
    u32 fs = (dfsr & 0xF) | ((dfsr >> 6) & 0x10);
    kf_last.n++;
    kf_last.dfar = dfar; kf_last.dfsr = dfsr; kf_last.pc = pc; kf_last.spsr = spsr;
    /* Translation and permission faults on user pages (demand paging,
     * copy-on-write via copyin/copyout) are routine; keep the rest, and
     * anything near NULL. */
    bool routine = (fs == 5 || fs == 7 || fs == 0xD || fs == 0xF) && dfar < 0xC0000000u;
    if (!routine || dfar < 0x10000u) {
        kf_bad.n++;
        kf_bad.dfar = dfar; kf_bad.dfsr = dfsr; kf_bad.pc = pc; kf_bad.spsr = spsr;
    }
}

/* "L<n> A<dfar> S<dfsr> P<pc> M<mode>" */
static char *kfault_line(char *p, char tag, const void *rec_) {
    const struct { u32 n, dfar, dfsr, pc, spsr; } *r = rec_;
    *p++ = tag; p = put_hex(p, r->n, 4);
    *p++ = ' '; *p++ = 'A'; p = put_hex(p, r->dfar, 8);
    *p++ = ' '; *p++ = 'S'; p = put_hex(p, r->dfsr, 3);
    *p++ = ' '; *p++ = 'P'; p = put_hex(p, r->pc, 8);
    *p++ = ' '; *p++ = 'M'; p = put_hex(p, r->spsr & 0x1F, 2);
    return p;
}

static void kda_status_draw(u32 ticks) {
    char buf[56], *p = buf;
    *p++ = 'T'; p = put_hex(p, ticks, 5);
    *p++ = ' '; *p++ = 'D'; p = put_hex(p, kda_total, 7);
    *p++ = ' '; *p++ = kda_last_write ? 'W' : 'R'; p = put_hex(p, kda_last_pa, 8);
    *p++ = ' '; *p++ = '@'; p = put_hex(p, kda_last_pc, 8);
    *p++ = ' '; *p++ = '='; p = put_hex(p, kda_last_val, 8);
    *p++ = ' '; *p++ = 'K'; p = put_hex(p, htl_crumb, 3);
    *p = 0;
    console_status(0, buf);

    /* three most frequent PAs: "PPPPPPPPxCCCC" */
    u32 used = 0;          /* bitmask over kda_top (32 slots) */
    p = buf;
    for (int k = 0; k < 3; k++) {
        int best = -1;
        for (u32 i = 0; i < KDA_TOP_SLOTS; i++)
            if (!(used & (1u << i)) && kda_top[i].count &&
                (best < 0 || kda_top[i].count > kda_top[best].count))
                best = (int)i;
        if (best < 0)
            break;
        used |= 1u << best;
        if (k)
            *p++ = ' ';
        p = put_hex(p, kda_top[best].pa, 8);
        *p++ = 'x';
        p = put_hex(p, kda_top[best].count > 0xFFFF ? 0xFFFF : kda_top[best].count, 4);
    }
    *p++ = ' '; *p++ = 'F'; p = put_hex(p, kern_fiq_delivered, 4);
    *p = 0;
    console_status(1, buf);

    /* "aACKS eEOIS E<VIC0 enable> R<VIC0 raw|soft> u<undefs> k<SGI kicks>" */
    u32 v[7];
    vic_debug(v);
    p = buf;
    *p++ = 'a'; p = put_hex(p, v[0], 4);
    *p++ = ' '; *p++ = 'e'; p = put_hex(p, v[1], 4);
    *p++ = ' '; *p++ = 'E'; p = put_hex(p, v[5], 8);
    *p++ = ' '; *p++ = 'R'; p = put_hex(p, v[6], 8);
    extern u32 kern_sgi_kicks;
    extern u32 kern_und_count;
    *p++ = ' '; *p++ = 'u'; p = put_hex(p, kern_und_count, 6);
    *p++ = ' '; *p++ = 'k'; p = put_hex(p, kern_sgi_kicks, 4);
    *p = 0;
    console_status(2, buf);

    extern void nand_status_line(char *buf);
    nand_status_line(buf);
    console_status(3, buf);

    extern void kps_status_line(char *buf, u32 now);
    kps_status_line(buf, ticks);
    console_status(4, buf);

    /* Display bring-up evidence: blocked IOKit drivers, IOKit probe/start
     * counts and the last display-related kernel line, CLCD/SPI traffic. */
    extern void kps_iok_line(char *buf, u32 now, char *buf2);
    char row_b[64];
    kps_iok_line(buf, ticks, row_b);
    console_status(5, buf);
    console_status(9, row_b);
    /* rows 6/12 (were the IOKit start counters and the PL080 DMA row):
     * SpringBoard's main thread and its first kernel-blocked thread */
    {
        extern void kps_sb_lines(char *a, char *b);
        char row_c[64];
        kps_sb_lines(row_c, row_b);
        console_status(12, row_c);
        console_status(6, row_b);
    }
    uart_tap_rows(buf, row_b);
    console_status(7, row_b);
    display_status_line(buf);
    console_status(8, buf);
    /* rows 10/11 were the boot-time IOKit node states: now the power path
     * (pm.c probes) and the Hold/Home/PMU wake inputs */
    extern void pm_trace_line(char *buf);
    pm_trace_line(buf);
    console_status(10, buf);
    input_button_line(row_b);
    console_status(11, row_b);

    {
        char *p = kfault_line(buf, 'L', &kf_last);
        *p = 0;
        console_status(13, buf);
    }
    extern void spi2_status_line(char *buf);     /* was: B (= L lately) */
    spi2_status_line(buf);
    console_status(14, buf);
    input_status_line(buf);
    console_status(15, buf);
    uart_tap_idle();
}

/* The kernel access being emulated (for periph.c's unknown-IO log). */
u32 htl_ctx_pc, htl_ctx_va, htl_ctx_mode;

/* Once per window group: which L1 entry was found overwritten, with what,
 * and in which context (recorded by kern_map_window). */
static void kern_win_report(void) {
    extern u32 kern_win_rec[16];
    static u32 shown;
    static const char *const kind[4] = { "zero", "L2tbl", "section", "?" };
    for (u32 g = 0; g < 4; g++) {
        u32 *r = &kern_win_rec[g * 4];
        if (!r[0] || (shown & (1u << g)))
            continue;
        shown |= 1u << g;
        kern_console();
        console_print("[WIN] grp ");
        console_print_hex(g);
        console_print(" L1[");
        console_print_hex(r[0]);
        console_print("]=");
        console_print_hex(r[1]);
        console_print(" ");
        console_print(r[1] ? kind[r[1] & 3] : kind[0]);
        console_print("\n TTBR1=");
        console_print_hex(r[2]);
        console_print(" SPSR=");
        console_print_hex(r[3]);
        console_print(" reinstalled\n");
    }
}

/* Fallback for faults raised by our own runtime: a fault whose PC is in our
 * image or whose address is in the device window.  Capture the TTBR1 entry
 * for the address before touching anything, force the window back in, say
 * what happened, then retry window faults (twice per PC at most) and freeze
 * on anything else so the screen keeps the evidence. */
static u32 kern_self_fault(u32 dfar, u32 dfsr, u32 fault_pc, u32 mode) {
    extern u8 __img_end[];
    static u32 seen_pc[4], seen_n[4], shown;

    u32 ttbr1;
    __asm__ volatile("mrc p15, 0, %0, c2, c0, 1" : "=r"(ttbr1));
    u32 tva = kern_table_va(ttbr1 & 0xFFFFC000u);
    u32 l1 = tva ? ((volatile u32 *)tva)[dfar >> 20] : 0xDEADDEAD;

    __asm__ volatile("bl kern_map_window_force"
                     ::: "r0", "r1", "r2", "r3", "r4", "r5", "r6", "r7",
                         "r8", "r9", "ip", "lr", "cc", "memory");

    bool in_win = dfar - IOWIN_IO_VA < 0x03000000u;
    u32 slot = 4;
    for (u32 i = 0; i < 4; i++)
        if (seen_pc[i] == fault_pc) { slot = i; break; }
    if (slot == 4 && shown < 4) {
        slot = shown++;
        seen_pc[slot] = fault_pc;
    }
    u32 hits = slot < 4 ? ++seen_n[slot] : 99;

    kern_console();
    console_print("[FLT] m=");
    console_print_hex(mode);
    console_print(" A=");
    console_print_hex(dfar);
    console_print(" S=");
    console_print_hex(dfsr);
    console_print("\n PC=");
    console_print_hex(fault_pc);
    console_print(" L1=");
    console_print_hex(l1);
    console_print("\n");

    if (in_win && hits <= 2)
        return fault_pc;            /* window was missing: retry */
    console_print("[FLT] halted\n");
    (void)__img_end;
    __asm__ volatile("cpsid if");
    for (;;)
        __asm__ volatile("wfi");
}

/* ── Kernel panic text ────────────────────────────────────────────────
 * XNU keeps everything kdb_printf prints (the "panic(cpu 0 caller ...)"
 * line and the backtrace) in debug_buf, and panic() points panicstr at
 * the format string; the UART copy can be hidden under the [SPIN] report.
 * Addresses from KERNEL.BIN's symbol table. */
#define K_PANICSTR       0xC01A522Cu
#define K_DEBUG_BUF      0xC01D13D0u     /* char *debug_buf */
#define K_DEBUG_BUF_PTR  0xC01D13D8u     /* char *debug_buf_ptr */

static u32 kstr_read(u32 va, char *out, u32 max) {
    u32 n = 0;
    while (n < max) {
        u32 w;
        if (!safe_rd32(va + n, &w))
            break;
        u32 i = (va + n) & 3;
        char c = (char)(w >> (8 * i));
        out[n++] = c;
        if (!c)
            break;
    }
    if (n == max)
        out[max - 1] = 0;
    return n;
}

static u32 kpanic_report(void) {
    char s[328];
    u32 rows = 0;
    u32 ps = safe_word(K_PANICSTR);
    if (ps) {
        kstr_read(ps, s, 47);
        console_print("panicstr: ");
        for (char *c = s; *c; c++)
            console_putc(*c >= 0x20 && *c < 0x7F ? *c : '.');
        console_print("\n");
        rows++;
    }
    extern void uart_tap_panic_line(char *out, u32 max);
    uart_tap_panic_line(s, 49);
    if (s[0]) {
        console_print("U:");
        console_print(s);
        console_print("\n");
        rows++;
    }
    u32 buf = safe_word(K_DEBUG_BUF), ptr = safe_word(K_DEBUG_BUF_PTR);
    if (!buf || ptr <= buf || ptr - buf > 0x100000)
        return rows;
    u32 from = ptr - buf > 320 ? ptr - 320 : buf, n = 0;
    for (u32 va = from; va < ptr && n < sizeof(s) - 1; va++) {
        u32 w;
        if (!safe_rd32(va, &w))
            break;
        char c = (char)(w >> (8 * (va & 3)));
        s[n++] = c == '\n' || (c >= 0x20 && c < 0x7F) ? c : '.';
    }
    s[n] = 0;
    /* the last 6 lines, each cut to 48 columns (14 log rows in all) */
    u32 start = n, lines = 0;
    while (start > 0 && lines < 6)
        if (s[--start] == '\n' && start + 1 < n)
            lines++;
    u32 col = 0;
    for (u32 i = start; i < n; i++) {
        if (s[i] == '\n') {
            if (col) {
                console_putc('\n');
                rows++;
            }
            col = 0;
        } else if (col < 48) {
            console_putc(s[i]);
            col++;
        }
    }
    if (col) {
        console_putc('\n');
        rows++;
    }
    return rows;
}

/* ── Spin report ──────────────────────────────────────────────────────
 * The kernel polling MMIO with IRQs masked shows up as traps continuing
 * while the heartbeat count stands still.  Keep the last 8 distinct trapped
 * accesses and, once the beat has been stuck for SPIN_TRAPS traps, clear
 * the top screen and print them (refreshed every 16K traps). */
#define SPIN_TRAPS 60000u
static struct { u32 pa, pc, val; bool w; u32 n; } spin_ring[8];
static u32 spin_head;

static void spin_note(u32 pa, bool w, u32 pc, u32 val, u32 spsr) {
    extern u32 kern_irq_taken;
    static u32 beat, base, reports;
    for (u32 i = 0; i < 8; i++)
        if (spin_ring[i].pa == pa && spin_ring[i].pc == pc && spin_ring[i].w == w) {
            spin_ring[i].val = val;
            spin_ring[i].n++;
            goto counted;
        }
    spin_head = (spin_head + 1) & 7;
    spin_ring[spin_head].pa = pa;
    spin_ring[spin_head].pc = pc;
    spin_ring[spin_head].val = val;
    spin_ring[spin_head].w = w;
    spin_ring[spin_head].n = 1;
counted:
    if (kern_irq_taken != beat) {
        beat = kern_irq_taken;
        base = kda_total;
        return;
    }
    u32 stuck = kda_total - base;
    if (stuck < SPIN_TRAPS || (stuck & 0x3FFF))
        return;
    reports++;
    console_home();
    console_print("[SPIN] beat ");
    console_print_hex(beat);
    console_print(" stuck, traps ");
    console_print_hex(stuck);
    console_print("\n SPSR ");
    console_print_hex(spsr);
    console_print(" crumb ");
    console_print_hex(htl_crumb);
    console_print(" #");
    console_print_hex(reports);
    console_print("\n");
    /* panic text first; the hot accesses fill the rest of the 14 rows */
    u32 left = 12 - kpanic_report();
    for (u32 k = 0; k < 8 && left; k++) {
        u32 i = (spin_head + 8 - k) & 7;
        if (!spin_ring[i].n)
            continue;
        left--;
        console_print(spin_ring[i].w ? "W " : "R ");
        console_print_hex(spin_ring[i].pa);
        console_print(" @");
        console_print_hex(spin_ring[i].pc);
        console_print(" =");
        console_print_hex(spin_ring[i].val);
        console_print(" n");
        console_print_hex(spin_ring[i].n);
        console_print("\n");
    }
    kda_status_draw(beat);
}

/* Returns the address to resume at, or 0 to chain to the kernel. */
u32 htl_kern_abort(u32 dfar, u32 dfsr, u32 *regs, u32 fault_pc) {
    static u32 n;
    u32 fs = (dfsr & 0xF) | ((dfsr >> 6) & 0x10);
    u32 spsr;
    __asm__ volatile("mrs %0, spsr" : "=r"(spsr));
    u32 mode = spsr & 0x1F;

    kern_win_report();
    und_followup("DA", mode, fault_pc);
    {
        extern u8 __img_end[];
        if (fault_pc - IMG_VA < (u32)__img_end - IMG_VA ||
            dfar - IOWIN_IO_VA < 0x03000000u)
            return kern_self_fault(dfar, dfsr, fault_pc, mode);
    }

    /* Only one kind of abort is ours: a precise external abort on a kernel
     * mapping of an S5L PA (nothing on the 3DS bus answers there).  A
     * translation fault is never ours, even at VA 0x38000000-0x3FFFFFFF:
     * under TTBCR.N=2 that is user address space, and the kernel touching
     * an unmapped user page there (copyout, demand paging) must reach its
     * own handler.  Everything else belongs to the kernel too. */
    u32 pa;
    bool ours;
    if (fs == 8) {
        pa = kern_va_to_pa(dfar);
        ours = pa >= S5L_IO_BASE && pa < S5L_IO_END;
    } else {
        pa = 0xFFFFFFFFu;
        ours = false;
    }
    /* FIQ-mode aborts can't go through the kernel's handler (it would treat
     * them as SVC faults); keep skipping those. */
    bool chain = !ours && mode != 0x11;

    if (!ours && mode != 0x10 && mode != 0x13) {
        static u32 exc_shown;
        if (exc_shown++ < 4) {
            kern_console();
            console_print("[EXC] DA mode=");
            console_print_hex(mode);
            console_print(" A=");
            console_print_hex(dfar);
            console_print(" PC=");
            console_print_hex(fault_pc);
            console_print("\n");
        }
    }

    /* Not ours (user-mode aborts always land here): hand it to the kernel
     * without touching the faulting code or data, whose page may well be
     * the one that is missing. */
    if (chain) {
        n++;
        if (mode != 0x10)
            kfault_note(dfar, dfsr, fault_pc, spsr);
        return 0;               /* hand the abort to _fleh_dataabt */
    }

    bool thumb = (spsr >> 5) & 1;
    u32 instr;
    u32 resume = fault_pc + (thumb ? 2 : 4);
    if (!safe_insn(fault_pc, thumb, &instr)) {
        kern_console();
        console_print("[DA] S5L access, unreadable PC=");
        console_print_hex(fault_pc);
        console_print("\n");
        return resume;
    }

    kern_console();
    if (n < 14) {
        console_print("A=");
        console_print_hex(dfar);
        console_print(" P=");
        console_print_hex(pa);
        console_print(" S=");
        console_print_hex(dfsr);
        console_print("\n PC=");
        console_print_hex(fault_pc);
        console_print(" I=");
        console_print_hex(instr);
        console_print("\n");
    }
    n++;

    htl_access_t access;
    access.address = pa;
    bool decoded = thumb ? kern_decode_thumb((u16)instr, regs, &access)
                         : htl_decode_instruction(instr, regs, &access);
    if (!decoded) {             /* undecodable (e.g. LDM/STM): skipped */
        static u32 undec_pc[4], undec_n;
        bool seen = false;
        for (u32 i = 0; i < undec_n; i++)
            seen |= undec_pc[i] == fault_pc;
        if (!seen && undec_n < 4) {
            undec_pc[undec_n++] = fault_pc;
            kern_console();
            console_print("[UNDEC] PC=");
            console_print_hex(fault_pc);
            console_print(" I=");
            console_print_hex(instr);
            console_print(" PA=");
            console_print_hex(pa);
            console_print("\n");
        }
        return resume;
    }
    if (!thumb)
        kern_apply_writeback(instr, regs);

    extern u32 kps_after_emul;
    kps_after_emul = 1;         /* next heartbeat sample: emulation cost */
    u32 result = 0;
    if (ours) {
        htl_ctx_pc = fault_pc;
        htl_ctx_va = dfar;
        htl_ctx_mode = mode;
        result = htl_dispatch(&access);
    }

    if (!access.is_write && access.reg_index <= 12)
        regs[access.reg_index] = result;

    kda_total++;
    spin_note(pa, access.is_write, fault_pc,
              access.is_write ? access.value : result, spsr);
    if ((kda_total & 0xFFF) == 0) {     /* still visible if IRQs stay off */
        extern u32 kern_irq_taken;
        kda_status_draw(kern_irq_taken);
    }
    kda_last_pa = pa;
    kda_last_pc = fault_pc;
    kda_last_write = access.is_write;
    kda_last_val = access.is_write ? access.value : result;
    kda_count_pa(pa);
    return resume;
}

/* ── IRQ bridge (called from kern_htl_irq_entry in IRQ mode) ─────────── */

#define GICC_IAR        IOREG32(0x17E0010C)
#define GICC_EOI        IOREG32(0x17E00110)
#define GICD_CLR_EN(n)  IOREG32(0x17E01180 + 4 * (n))

u32 kern_irq_taken;          /* 100 Hz heartbeats */
u32 kern_irq_chained;        /* emulated VIC IRQs handed to _fleh_irq */
u32 kern_sgi_kicks;          /* SGI 1 deliveries taken */

/* Redraw the pinned status rows from a non-IRQ path. */
void htl_status_redraw(void) {
    kda_status_draw(kern_irq_taken);
}

#define GICD_SGIR       IOREG32(0x17E01F00)

/* Ask for prompt delivery of an emulated VIC interrupt: an SGI to this CPU
 * is taken as soon as the kernel runs with IRQs unmasked. */
void vic_kick(void) {
    GICD_SGIR = (2u << 24) | KICK_SGI_ID;
}

/* What kern_htl_irq_entry should do after an MPCore IRQ:
 *   0  return to the interrupted code
 *   1  enter the kernel's FIQ handler (timer, VIC line 7)
 *   2  enter the kernel's IRQ vector (_fleh_irq) for an emulated VIC IRQ */
u32 htl_kern_irq(u32 spsr_irq, u32 lr_irq) {
    static bool pending, shown_fiq, shown_masked, shown_irq;
    und_followup("IRQ", spsr_irq, 0);

    u32 iar = GICC_IAR;
    u32 id = iar & 0x3FF;
    if (id == 1023)
        return 0;

    if (id == KICK_SGI_ID) {
        kern_sgi_kicks++;
        GICC_EOI = iar;
    } else if (id == 29) {
        bool due = timer_heartbeat();
        GICC_EOI = iar;
        extern void kps_sample(u32 pc, u32 spsr);
        kps_sample(lr_irq - 4, spsr_irq);

        kern_irq_taken++;
        display_mirror_step();
        input_poll(kern_irq_taken);
        pl080_tick();
        extern void spi_tick(void);
        spi_tick();
        if (kern_irq_taken % 10 == 0) {
            kda_status_draw(kern_irq_taken);
            kern_win_report();
        }
        if (due)
            pending = true;
    } else {
        GICD_CLR_EN(id >> 5) = 1u << (id & 31);
        GICC_EOI = iar;
        console_print("[IRQ] masked stray id ");
        console_print_hex(id);
        console_print("\n");
        return 0;
    }

    if (pending) {
        if (!(spsr_irq & 0x40)) {
            pending = false;
            kern_fiq_delivered++;
            if (!shown_fiq) {
                shown_fiq = true;
                console_print("[IRQ] first FIQ -> kernel\n");
            }
            if (vic_irq_pending())
                vic_kick();     /* IRQ waiting too: come back after the FIQ */
            return 1;
        }
        if (!shown_masked) {    /* kernel has FIQ masked: a later beat */
            shown_masked = true;
            console_print("[IRQ] FIQ masked, CPSR=");
            console_print_hex(spsr_irq);
            console_print("\n");
        }
    }

    /* An IRQ exception was taken, so the kernel had IRQs unmasked. */
    if (vic_irq_pending()) {
        kern_irq_chained++;
        if (!shown_irq) {
            shown_irq = true;
            console_print("[IRQ] first VIC IRQ -> kernel\n");
        }
        return 2;
    }
    return 0;
}

/* Log prefetch aborts taken under the kernel (first few). */
void htl_kern_pabt_log(u32 lr_abt, u32 ifsr, u32 spsr) {
    extern u32 kern_fiq_target;
    static u32 n;
    und_followup("PAB", spsr, lr_abt - 4);
    if (n++ >= 3)
        return;
    kern_console();
    console_print("PAB PC=");
    console_print_hex(lr_abt - 4);
    console_print(" FS=");
    console_print_hex(ifsr);
    console_print("\n SPSR=");
    console_print_hex(spsr);
    console_print(" FIQ r9=");
    console_print_hex(kern_fiq_target);
    console_print("\n");
}
