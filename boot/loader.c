/*
 * loader.c — Mach-O kernelcache loader for iOS 3DS bootloader
 *
 * Parses a decrypted 32-bit ARM Mach-O (iPhone OS 1.x kernelcache),
 * copies its segments to their virtual addresses (which the MMU has
 * remapped to FCRAM), and provides a jump-to-kernel function.
 */

#include "loader.h"
#include "bootargs.h"
#include "console.h"

/* ── Helpers ────────────────────────────────────────────────────────── */

static void mem_copy(void *dst, const void *src, u32 n) {
    u8 *d = (u8 *)dst;
    const u8 *s = (const u8 *)src;
    for (u32 i = 0; i < n; i++)
        d[i] = s[i];
}

static void mem_zero(void *dst, u32 n) {
    u8 *d = (u8 *)dst;
    for (u32 i = 0; i < n; i++)
        d[i] = 0;
}

/* ── Mach-O parser ──────────────────────────────────────────────────── */

bool loader_parse_macho(const void *data, size_t size, kernel_info_t *info) {
    if (size < sizeof(struct mach_header)) {
        console_print("[LOADER] ERROR: data too small for Mach-O header\n");
        return false;
    }

    const struct mach_header *mh = (const struct mach_header *)data;

    if (mh->magic != MH_MAGIC) {
        console_print("[LOADER] ERROR: bad magic ");
        console_print_hex(mh->magic);
        console_print("\n");
        return false;
    }

    if (mh->cputype != CPU_TYPE_ARM) {
        console_print("[LOADER] ERROR: not ARM (cputype=");
        console_print_hex(mh->cputype);
        console_print(")\n");
        return false;
    }

    console_print("[LOADER] Mach-O ARM");
    if (mh->cpusubtype == CPU_SUBTYPE_V6)
        console_print("v6");
    console_print(", ");
    console_print_hex(mh->ncmds);
    console_print(" load commands\n");

    info->entry_point = 0;
    info->load_base   = 0xFFFFFFFF;
    info->load_end    = 0;

    const u8 *cmd_ptr = (const u8 *)data + sizeof(struct mach_header);

    for (u32 i = 0; i < mh->ncmds; i++) {
        u32 cmd     = *(u32 *)(cmd_ptr + 0);
        u32 cmdsize = *(u32 *)(cmd_ptr + 4);

        if (cmd == LC_SEGMENT) {
            const struct segment_command *seg =
                (const struct segment_command *)cmd_ptr;

            console_print("  SEG ");
            /* Print first 16 chars of segname */
            for (u32 j = 0; j < 16 && seg->segname[j]; j++) {
                char c[2] = { seg->segname[j], '\0' };
                console_print(c);
            }
            console_print("  vm=");
            console_print_hex(seg->vmaddr);
            console_print(" sz=");
            console_print_hex(seg->vmsize);
            console_print("\n");

            /* Skip __PAGEZERO (vmaddr=0, filesize=0) — it is not loaded
               and including it causes the vector scanner to start at VA 0,
               where it finds the template in the raw staging copy instead
               of the loaded kernel segments. */
            if (seg->filesize > 0 || seg->vmaddr != 0) {
                if (seg->vmaddr < info->load_base)
                    info->load_base = seg->vmaddr;
            }
            if (seg->vmaddr + seg->vmsize > info->load_end)
                info->load_end = seg->vmaddr + seg->vmsize;
        }
        else if (cmd == LC_UNIXTHREAD) {
            /* Thread state: flavor(4) count(4) regs[count]
             * For ARM: regs[15] = PC = entry point */
            const u32 *ts = (const u32 *)(cmd_ptr + 8);
            u32 flavor UNUSED = ts[0];
            u32 count  = ts[1];
            if (count >= 16) {
                info->entry_point = ts[2 + 15];  /* regs[15] = PC */
                console_print("  Entry point: ");
                console_print_hex(info->entry_point);
                console_print("\n");
            }
        }

        cmd_ptr += cmdsize;
    }

    if (info->entry_point == 0) {
        console_print("[LOADER] WARNING: no entry point found\n");
    }

    return true;
}

/* ── Segment loader ─────────────────────────────────────────────────── */

bool loader_load_segments(const void *data, size_t size) {
    const struct mach_header *mh = (const struct mach_header *)data;
    if (mh->magic != MH_MAGIC) return false;

    const u8 *cmd_ptr = (const u8 *)data + sizeof(struct mach_header);

    for (u32 i = 0; i < mh->ncmds; i++) {
        u32 cmd     = *(u32 *)(cmd_ptr + 0);
        u32 cmdsize = *(u32 *)(cmd_ptr + 4);

        if (cmd == LC_SEGMENT) {
            const struct segment_command *seg =
                (const struct segment_command *)cmd_ptr;

            /* Copy file data to virtual address */
            if (seg->filesize > 0 && seg->vmaddr != 0) {
                mem_copy((void *)(uintptr_t)seg->vmaddr,
                         (const u8 *)data + seg->fileoff,
                         seg->filesize);
            }

            /* Zero any remaining space (vmsize > filesize) */
            if (seg->vmsize > seg->filesize && seg->vmaddr != 0) {
                mem_zero((void *)(uintptr_t)(seg->vmaddr + seg->filesize),
                         seg->vmsize - seg->filesize);
            }
        }

        cmd_ptr += cmdsize;
    }

    console_print("[LOADER] Segments loaded\n");
    return true;
}

/* ── Kernel vector patching ─────────────────────────────────────────── */

/*
 * Search the loaded kernel for the exception vector template and patch
 * the data abort and undefined instruction literal-pool entries to
 * point to our kern_htl handlers instead of the kernel's.
 *
 * The ARM vector table is 8 consecutive `ldr pc, [pc, #imm]`
 * instructions (encoding 0xE59FFxxx) followed by 8 literal-pool words
 * containing the actual handler addresses.
 *
 * Vector layout:
 *   +0x00 Reset        +0x20 reset_handler
 *   +0x04 Undef        +0x24 undef_handler
 *   +0x08 SVC          +0x28 svc_handler
 *   +0x0C Prefetch Abt +0x2C pabt_handler
 *   +0x10 Data Abort   +0x30 dabt_handler   ← patch this
 *   +0x14 Reserved     +0x34 (reserved)
 *   +0x18 IRQ          +0x38 irq_handler
 *   +0x1C FIQ          +0x3C fiq_handler
 */

extern u32 kern_da_orig_addr;
extern u32 kern_undef_orig_addr;
extern void kern_htl_da_entry(void);
extern void kern_htl_undef_entry(void);
extern u32 kern_pabt_orig_addr;
extern void kern_htl_pabt_entry(void);
extern void kern_htl_irq_entry(void);

bool loader_patch_kernel_vectors(const kernel_info_t *kinfo) {
    u32 *start = (u32 *)(uintptr_t)kinfo->load_base;
    u32 *end   = (u32 *)(uintptr_t)(kinfo->load_end - 64);

    for (u32 *p = start; p < end; p++) {
        /* Count consecutive ldr pc, [pc, #imm] instructions.
         * Encoding: 0xE59FF000 | imm12  →  mask 0xFFFFF000 */
        int count = 0;
        for (int i = 0; i < 8; i++) {
            if ((p[i] & 0xFFFFF000) == 0xE59FF000)
                count++;
        }
        if (count < 5)
            continue;

        /* The reset vector (p[0]) MUST be a ldr pc — this prevents
           matching at a misaligned offset due to zero-padding before
           the real table. */
        if ((p[0] & 0xFFFFF000) != 0xE59FF000)
            continue;

        /* Verify the data abort slot (p[4]) is a ldr pc */
        if ((p[4] & 0xFFFFF000) != 0xE59FF000)
            continue;

        console_print("[LOADER] Found vector template at ");
        console_print_hex((u32)p);
        console_print("\n");

        /* For each ldr pc, [pc, #imm]:  target = &instr + 8 + imm12
         * The literal pool word at that address holds the handler VA. */

        /* Data abort (slot 4, offset 0x10) */
        {
            u32 imm12 = p[4] & 0xFFF;
            u32 *literal = (u32 *)((u8 *)&p[4] + 8 + imm12);
            kern_da_orig_addr = *literal;
            *literal = (u32)kern_htl_da_entry;

            console_print("  DA: kernel=");
            console_print_hex(kern_da_orig_addr);
            console_print(" -> ours=");
            console_print_hex((u32)kern_htl_da_entry);
            console_print("\n");
        }

        /* Undefined instruction (slot 1, offset 0x04) */
        if ((p[1] & 0xFFFFF000) == 0xE59FF000) {
            u32 imm12 = p[1] & 0xFFF;
            u32 *literal = (u32 *)((u8 *)&p[1] + 8 + imm12);
            kern_undef_orig_addr = *literal;
            *literal = (u32)kern_htl_undef_entry;

            console_print("  Undef: kernel=");
            console_print_hex(kern_undef_orig_addr);
            console_print(" -> ours=");
            console_print_hex((u32)kern_htl_undef_entry);
            console_print("\n");
        }

        /* Prefetch abort (slot 3, offset 0x0C) */
        if ((p[3] & 0xFFFFF000) == 0xE59FF000) {
            u32 imm12 = p[3] & 0xFFF;
            u32 *literal = (u32 *)((u8 *)&p[3] + 8 + imm12);
            kern_pabt_orig_addr = *literal;
            *literal = (u32)kern_htl_pabt_entry;
        }

        /* IRQ (slot 6): MPCore GIC IRQ -> bridge into the kernel's FIQ
         * handler or, for emulated VIC IRQs, its own IRQ vector. */
        if ((p[6] & 0xFFFFF000) == 0xE59FF000) {
            extern u32 kern_irq_orig_addr;
            u32 imm12 = p[6] & 0xFFF;
            u32 *literal = (u32 *)((u8 *)&p[6] + 8 + imm12);
            kern_irq_orig_addr = *literal;
            *literal = (u32)kern_htl_irq_entry;
            console_print("  IRQ -> ours=");
            console_print_hex((u32)kern_htl_irq_entry);
            console_print("\n");
        }

        console_print("[LOADER] Vectors patched\n");
        return true;
    }

    console_print("[LOADER] WARNING: vector template not found!\n");
    return false;
}

/* ── Kernel jump ────────────────────────────────────────────────────── */

NORETURN void loader_jump_to_kernel(u32 entry, void *boot_args) {
    console_print("[LOADER] Jumping to kernel at ");
    console_print_hex(entry);
    console_print("\n");

    /* Pre-warm the TLB for addresses the kernel needs during its
     * TTBR transition.  The kernel changes TTBR to topOfKernelData
     * BEFORE building page tables there; the MMU resolves writes to
     * that area via cached TLB entries from our tables.  A volatile
     * read populates the emulator's software TLB for the whole 1MB
     * section so the kernel can zero and rebuild the page table. */
    {
        boot_args_t *ba = (boot_args_t *)boot_args;
        volatile u32 *p = (volatile u32 *)(uintptr_t)ba->topOfKernelData;
        (void)*p;
    }

    /* Enter the kernel with the D-cache OFF.  Its start code zeroes and
     * rebuilds the page tables with plain stores and only enables the
     * D-cache afterwards; the ARM11 table walker reads RAM, so tables
     * left dirty in the D-cache leave everything (vectors included)
     * unmapped after the kernel's TLBIALL. */
    /* Grant CP10/CP11 (VFP) full access, as iBoot does, and hand over a
     * clean FPEXC (EN clear) so the kernel's lazy VFP enable works. */
    {
        u32 cpacr;
        __asm__ volatile("mrc p15, 0, %0, c1, c0, 2" : "=r"(cpacr));
        cpacr |= 0xFu << 20;
        __asm__ volatile("mcr p15, 0, %0, c1, c0, 2\n"
                         "mcr p15, 0, %1, c7, c5, 4" :: "r"(cpacr), "r"(0));
        u32 fpsid, fpexc;
        __asm__ volatile("mrc p10, 7, %0, c0, c0, 0" : "=r"(fpsid));
        __asm__ volatile("mrc p10, 7, %0, c8, c0, 0" : "=r"(fpexc));
        console_print("[VFP] FPSID=");
        console_print_hex(fpsid);
        console_print(" FPEXC=");
        console_print_hex(fpexc);
        console_print(" -> 0\n");
        /* EN clear for the kernel's lazy enable; EX (a pending VFP
         * exception left by the firmware) must not survive, since the
         * kernel never clears it and every VFP instruction would bounce. */
        __asm__ volatile("mcr p10, 7, %0, c8, c0, 0" :: "r"(0));
    }

    __asm__ volatile(
        "cpsid if                       \n"
        "mrc   p15, 0, r2, c1, c0, 0    \n"
        "bic   r2, r2, #(1 << 2)        \n"   /* SCTLR.C = 0 */
        "mcr   p15, 0, r2, c1, c0, 0    \n"
        "mov   r2, #0                   \n"
        "mcr   p15, 0, r2, c7, c14, 0   \n"   /* clean+invalidate D$ */
        "mcr   p15, 0, r2, c7, c10, 4   \n"   /* DSB */
        "mcr   p15, 0, r2, c7, c5, 0    \n"   /* invalidate I$ */
        "mcr   p15, 0, r2, c7, c5, 6    \n"   /* flush branch predictor */
        "mcr   p15, 0, r2, c7, c5, 4    \n"   /* ISB */
        "mov   r0, %0                   \n"
        "bx    %1                       \n"
        :: "r"(boot_args), "r"(entry)
        : "r0", "r2", "memory"
    );

    __builtin_unreachable();
}
