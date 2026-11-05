/*
 * mmu.c — ARMv6 MMU setup for iOS 3DS bootloader
 *
 * Builds L1/L2 page tables that remap 3DS physical memory into the
 * address space the iPhone OS kernel expects (S5L8900 layout), then
 * enables the MMU with caches.
 *
 * Memory map summary:
 *   virt 0x08000000-0x0FFFFFFF -> phys 0x20000000-0x27FFFFFF  (iPhone DRAM)
 *   virt 0x0FE00000            -> phys 0x20100000             (kernel FB override)
 *   virt 0x10000000-0x10FFFFFF -> same (3DS IO, identity)
 *   virt 0x17E00000            -> same (MPCore private periphs)
 *   virt 0x18000000-0x185FFFFF -> same (VRAM, identity)
 *   virt 0x20000000-0x27FFFFFF -> same (FCRAM, identity)
 *   virt 0x38000000-0x3FFFFFFF -> FAULT (S5L8900 IO trap for HTL)
 *   virt 0xFFFF0000            -> vectors page in FCRAM
 */

#include "mmu.h"
#include "3ds_hw.h"
#include "bootargs.h"
#include "console.h"
#include "iowin.h"

u32 iowin_on;

/* ── L2 pool allocator ──────────────────────────────────────────────── */

static u32 l2_pool_next;

static void mem_zero(void *dst, u32 bytes) {
    u32 *p = (u32 *)dst;
    u32 words = bytes >> 2;
    for (u32 i = 0; i < words; i++)
        p[i] = 0;
}

/* Allocate a 1KB L2 page table from the pool. Returns NULL if exhausted. */
static u32 *l2_alloc(void) {
    if (l2_pool_next >= L2_POOL_BASE + L2_POOL_SIZE)
        return NULL;
    u32 *tbl = (u32 *)l2_pool_next;
    l2_pool_next += L2_TABLE_SIZE;
    mem_zero(tbl, L2_TABLE_SIZE);
    return tbl;
}

/* ── Public mapping functions ───────────────────────────────────────── */

void mmu_map_section(u32 virt, u32 phys, u32 flags) {
    vu32 *l1 = (vu32 *)L1_TABLE_BASE;
    u32 idx = virt >> 20;
    l1[idx] = (phys & 0xFFF00000) | flags;
}

void mmu_map_page(u32 virt, u32 phys, u32 flags) {
    vu32 *l1 = (vu32 *)L1_TABLE_BASE;
    u32 l1_idx = virt >> 20;
    u32 l2_idx = (virt >> 12) & 0xFF;

    u32 l1_entry = l1[l1_idx];
    u32 *l2;

    if ((l1_entry & 3) == 1) {
        /* Coarse page table already exists */
        l2 = (u32 *)(l1_entry & 0xFFFFFC00);
    } else {
        /* Allocate a new L2 table */
        l2 = l2_alloc();
        if (!l2) {
            console_print("[MMU] ERROR: L2 pool exhausted!\n");
            return;
        }
        /* L1 coarse descriptor: [31:10] = L2 base, [1:0] = 01 */
        l1[l1_idx] = ((u32)l2 & 0xFFFFFC00) | L1_COARSE;
    }

    l2[l2_idx] = (phys & 0xFFFFF000) | flags;
}

void mmu_unmap(u32 virt) {
    vu32 *l1 = (vu32 *)L1_TABLE_BASE;
    l1[virt >> 20] = L1_FAULT;
}

/* ── MMU initialisation ─────────────────────────────────────────────── */

void mmu_init(u32 fcram_size) {
    /* Zero the L1 table (every entry = translation fault) */
    mem_zero((void *)L1_TABLE_BASE, L1_TABLE_SIZE);
    /* Zero and reset the L2 pool */
    mem_zero((void *)L2_POOL_BASE, L2_POOL_SIZE);
    l2_pool_next = L2_POOL_BASE;

    console_print("[MMU] Building page tables...\n");

    /* ── 0. S5L8900 low memory: VA 0x00000000-0x07FFFFFF ──────────── */
    /* Map the full range below iPhone DRAM as an alias of FCRAM.
       The S5L8900 has BootROM/SRAM here; the kernel may access it. */
    for (u32 i = 0; i < 128; i++)
        mmu_map_section(i * 0x100000, FCRAM_BASE + i * 0x100000, SECTION_NORMAL);

    /* ── 1. Identity-map FCRAM (128 MB on O3DS, 256 MB on N3DS) ───── */
    for (u32 a = FCRAM_BASE; a < FCRAM_BASE + fcram_size; a += 0x100000)
        mmu_map_section(a, a, SECTION_NORMAL);

    /* Override framebuffer sections as non-cacheable so the GPU
       (which reads physical FCRAM directly) sees writes immediately. */
    mmu_map_section(0x20100000, 0x20100000, SECTION_UNCACHED);
    mmu_map_section(0x20200000, 0x20200000, SECTION_UNCACHED);

    /* ── 2. iPhone DRAM view: virt 0x08-0x0F -> phys 0x20-0x27 ────── */
    for (u32 i = 0; i < 128; i++) {
        u32 v = 0x08000000 + i * 0x100000;
        u32 p = FCRAM_BASE  + i * 0x100000;
        mmu_map_section(v, p, SECTION_NORMAL);
    }

    /* Override: kernel framebuffer at 0x0FE00000 -> real FB 0x20100000 */
    mmu_map_section(0x0FE00000, 0x20100000, SECTION_UNCACHED);

    /* ── 3. 3DS IO registers: 0x10000000-0x10FFFFFF ────────────────── */
    for (u32 a = 0x10000000; a < 0x11000000; a += 0x100000)
        mmu_map_section(a, a, SECTION_DEVICE);

    /* ── 4. MPCore private peripherals: 0x17E00000 ─────────────────── */
    mmu_map_section(0x17E00000, 0x17E00000, SECTION_DEVICE);

    /* ── 5. VRAM: 0x18000000-0x18FFFFFF ─────────────────────────────── */
    for (u32 a = 0x18000000; a < 0x19000000; a += 0x100000)
        mmu_map_section(a, a, SECTION_UNCACHED);

    /* ── 6. XNU kernel virtual mapping: 0xC0000000+ ──────────────────
     *
     * The iPhone OS kernel is linked at virtual 0xC0000000.  On the
     * S5L8900 the kernel is loaded at physical 0x09000000 (DRAM base
     * 0x08000000 + 16 MB).  With our DRAM remap that physical address
     * lives at real FCRAM 0x21000000.
     *
     * Map the full kernel memSize range: virtBase → physBase for
     * (FCRAM_end − physBase) bytes.  The kernel uses topOfKernelData
     * (0x22000000 = virtBase + 16 MB) to build its page tables, so
     * the mapping MUST extend past 16 MB or the kernel data-aborts
     * the first time it writes to its own L1 table.
     *
     * Old 3DS: 112 MB (0xC0000000-0xC6FFFFFF → 0x21000000-0x27FFFFFF)
     * New 3DS: 240 MB (0xC0000000-0xCEFFFFFF → 0x21000000-0x2FFFFFFF) */
    {
        u32 kern_mb = ((FCRAM_BASE + fcram_size) - BOOTARGS_PHYS_BASE) >> 20;
        for (u32 i = 0; i < kern_mb; i++) {
            u32 v = BOOTARGS_VIRT_BASE + i * 0x100000;
            u32 p = BOOTARGS_PHYS_BASE + i * 0x100000;
            mmu_map_section(v, p, SECTION_NORMAL);
        }
    }

    /* ── 7. S5L8900 peripheral range: 0x38-0x3F → FAULT ────────────── */
    /* Already zero (fault) from mem_zero — nothing to do.  Accesses
       here trigger a Data Abort that the HTL intercepts. */

    /* ── 8. Exception vectors at 0xFFFF0000 ────────────────────────── */
    {
        extern u32 exception_vectors;   /* defined in abort.S */
        u32 vec_phys = kva2pa((u32)&exception_vectors) & 0xFFFFF000;
        mmu_map_page(0xFFFF0000, vec_phys, L2_PAGE_NORMAL);
    }

    /* ── 9. Device window (iowin.h), same layout XNU's TTBR1 gets ───── */
    for (u32 i = 0; i < 16; i++)
        mmu_map_section(IOWIN_IO_VA + i * 0x100000, 0x10000000 + i * 0x100000,
                        SECTION_DEVICE);
    mmu_map_section(IOWIN_MP_VA, 0x17E00000, SECTION_DEVICE);
    for (u32 i = 0; i < 8; i++)
        mmu_map_section(IOWIN_VRAM_VA + i * 0x100000, 0x18000000 + i * 0x100000,
                        SECTION_UNCACHED);
    for (u32 i = 0; i < 16; i++)
        mmu_map_section(IOWIN_LOW_VA + i * 0x100000, 0x20000000 + i * 0x100000,
                        i == 8 ? SECTION_NORMAL : SECTION_UNCACHED);

    console_print("[MMU] Page tables built\n");
    console_print("  FCRAM identity:  0x20000000-");
    console_print_hex(FCRAM_BASE + fcram_size - 1);
    console_print("\n");
    console_print("  iPhone DRAM:     0x08000000-0x0FFFFFFF\n");
    console_print("  Kernel FB:       0x0FE00000 -> 0x20100000\n");
    {
        u32 kern_mb = ((FCRAM_BASE + fcram_size) - BOOTARGS_PHYS_BASE) >> 20;
        console_print("  Kernel virt:     0xC0000000-");
        console_print_hex(BOOTARGS_VIRT_BASE + kern_mb * 0x100000 - 1);
        console_print(" (");
        console_print_hex(kern_mb);
        console_print(" MB)\n");
    }
    console_print("  S5L8900 IO trap: 0x38000000-0x3FFFFFFF\n");
    console_print("  Vectors:         0xFFFF0000\n");

    /* ── Enable MMU ─────────────────────────────────────────────────── */

    /* TTBR0: L1 table base with inner/outer cacheable attributes
       Bit 0 = IRGN[0], Bit 3 = RGN[0] (outer WB-WA) */
    u32 ttbr0 = L1_TABLE_BASE | (1 << 0) | (1 << 3);
    __asm__ volatile("mcr p15, 0, %0, c2, c0, 0" :: "r"(ttbr0));

    /* TTBCR = 0: use TTBR0 for all addresses (N = 0) */
    __asm__ volatile("mcr p15, 0, %0, c2, c0, 2" :: "r"(0));

    /* DACR: Domain 0 = Manager (bypass AP checks; translation faults
       still fire for unmapped pages — exactly what the HTL needs). */
    u32 dacr = 0x3;
    __asm__ volatile("mcr p15, 0, %0, c3, c0, 0" :: "r"(dacr));

    /* Invalidate TLBs */
    __asm__ volatile("mcr p15, 0, %0, c8, c7, 0" :: "r"(0));  /* TLBIALL */

    /* Barriers */
    __asm__ volatile("mcr p15, 0, %0, c7, c10, 4" :: "r"(0)); /* DSB */
    __asm__ volatile("mcr p15, 0, %0, c7, c5, 4"  :: "r"(0)); /* ISB */

    /* SCTLR: enable MMU, data cache, instruction cache, high vectors */
    u32 sctlr;
    __asm__ volatile("mrc p15, 0, %0, c1, c0, 0" : "=r"(sctlr));
    sctlr |= (1 << 0);    /* M — MMU enable */
    sctlr |= (1 << 2);    /* C — Data cache */
    sctlr |= (1 << 12);   /* I — Instruction cache */
    sctlr |= (1 << 13);   /* V — High exception vectors (0xFFFF0000) */
    __asm__ volatile("mcr p15, 0, %0, c1, c0, 0" :: "r"(sctlr));

    /* ISB so the next fetch goes through the MMU */
    __asm__ volatile("mcr p15, 0, %0, c7, c5, 4" :: "r"(0));

    iowin_on = 1;
    console_print("[MMU] Enabled (M=1 C=1 I=1 V=1), device window at 0x80000000\n");
}

/* ── Kernel L1 table pre-population ────────────────────────────────────
 *
 * Write entries into the kernel's own L1 page table at KERN_L1_TABLE_BASE
 * (= topOfKernelData = 0x22000000) so that:
 *
 *   - Exception vectors at 0xFFFF0000 are reachable immediately after
 *     the kernel installs its TTBR, even before it maps its own vectors.
 *
 *   - Kernel text/data at 0xC0000000+ are mapped via section descriptors
 *     matching the range the kernel expects from memSize.
 *
 * The kernel's startup code may zero and rebuild this table, in which
 * case our entries are overwritten — this is a best-effort safety net.
 * The primary fix for the data abort is the correct mapping size in
 * mmu_init() above.
 * ──────────────────────────────────────────────────────────────────── */

void mmu_prepopulate_kernel_l1(u32 fcram_size) {
    vu32 *kl1 = (vu32 *)KERN_L1_TABLE_BASE;

    console_print("[MMU] Pre-populating kernel L1 at ");
    console_print_hex(KERN_L1_TABLE_BASE);
    console_print("\n");

    /* ── 1. Exception vectors: 0xFFFF0000 ──────────────────────────── */
    {
        extern u32 exception_vectors;   /* defined in abort.S */
        u32 vec_phys = kva2pa((u32)&exception_vectors) & 0xFFFFF000;

        /* Zero the L2 table for the vectors 1MB region (0xFFF00000) */
        u32 *l2 = (u32 *)KERN_L2_VEC_BASE;
        mem_zero(l2, L2_TABLE_SIZE);

        /* L2 index for 0xFFFF0000 within the 0xFFF00000 section:
         * (0xFFFF0000 >> 12) & 0xFF = 0xF0 */
        l2[0xF0] = (vec_phys & 0xFFFFF000) | L2_PAGE_NORMAL;

        /* L1 coarse page table descriptor at index 0xFFF */
        kl1[0xFFF] = (KERN_L2_VEC_BASE & 0xFFFFFC00) | L1_COARSE;

        console_print("  Vectors L2 at ");
        console_print_hex(KERN_L2_VEC_BASE);
        console_print(" -> phys ");
        console_print_hex(vec_phys);
        console_print("\n");
    }

    /* ── 2. Kernel text/data: 0xC0000000+ → physBase+ ──────────────── */
    {
        u32 kern_mb = ((FCRAM_BASE + fcram_size) - BOOTARGS_PHYS_BASE) >> 20;
        for (u32 i = 0; i < kern_mb; i++) {
            u32 phys = BOOTARGS_PHYS_BASE + i * 0x100000;
            kl1[0xC00 + i] = (phys & 0xFFF00000) | SECTION_NORMAL;
        }
        console_print("  Kernel sections:  0xC00-");
        console_print_hex(0xC00 + kern_mb - 1);
        console_print(" (");
        console_print_hex(kern_mb);
        console_print(" entries)\n");
    }

    /* ── 3. GPU framebuffer: 0xBE100000 → phys 0x20100000 ─────────── */
    /* Map the 3DS top-screen framebuffer (400×240 RGB8 = 288KB) into
     * kernel virtual space as uncached memory. This lets kernel code
     * blit directly to the GPU framebuffer for display output.
     * Also map the bottom screen FB and GPU IO registers. */
    kl1[0xBE1] = (FB_TOP_ADDR & 0xFFF00000) | SECTION_UNCACHED;
    kl1[0xBE2] = (FB_BOT_ADDR & 0xFFF00000) | SECTION_UNCACHED;
    /* GPU LCD registers at phys 0x10400000 → kernel VA 0xBE400000 */
    kl1[0xBE4] = (0x10400000) | SECTION_DEVICE;
    console_print("  GPU FB top:       0xBE100000 -> ");
    console_print_hex(FB_TOP_ADDR);
    console_print("\n");
    console_print("  GPU FB bot:       0xBE200000 -> ");
    console_print_hex(FB_BOT_ADDR);
    console_print("\n");
    console_print("  GPU registers:    0xBE400000 -> 0x10400000\n");

    /* ── 4. S5L8900 low memory: VA 0x00000000-0x07FFFFFF ──────────── */
    for (u32 i = 0; i < 128; i++)
        kl1[i] = ((FCRAM_BASE + i * 0x100000) & 0xFFF00000) | SECTION_NORMAL;
    console_print("  Low memory:       0x00000000-0x07FFFFFF\n");

    /* ── 5. FCRAM identity: VA 0x20000000+ ─────────────────────────
     * Our bootloader handlers (htl_handle_abort, etc.) are linked at
     * FCRAM addresses. After kernel TTBR switch they must remain
     * accessible for exception handling to work. */
    {
        u32 fcram_mb = fcram_size >> 20;
        for (u32 i = 0; i < fcram_mb; i++)
            kl1[0x200 + i] = ((FCRAM_BASE + i * 0x100000) & 0xFFF00000) | SECTION_NORMAL;
        console_print("  FCRAM identity:   0x200-");
        console_print_hex(0x200 + fcram_mb - 1);
        console_print("\n");
    }

    /* ── 6. iPhone DRAM: VA 0x08000000-0x0FFFFFFF → FCRAM ──────── */
    for (u32 i = 0; i < 128; i++)
        kl1[0x080 + i] = ((FCRAM_BASE + i * 0x100000) & 0xFFF00000) | SECTION_NORMAL;
    console_print("  iPhone DRAM:      0x08000000-0x0FFFFFFF\n");

    /* ── 7. 3DS IO registers: 0x10000000-0x10FFFFFF ────────────── */
    for (u32 i = 0; i < 16; i++)
        kl1[0x100 + i] = ((0x10000000 + i * 0x100000) & 0xFFF00000) | SECTION_DEVICE;

    /* ── 8. VRAM: 0x18000000-0x18FFFFFF ────────────────────────── */
    for (u32 i = 0; i < 16; i++)
        kl1[0x180 + i] = ((0x18000000 + i * 0x100000) & 0xFFF00000) | SECTION_UNCACHED;

    /* ── 3. Flush to physical memory ───────────────────────────────── */
    /* The hardware page-table walker may bypass the D-cache on some
     * implementations.  Clean the entire D-cache and barrier to ensure
     * the entries are visible in FCRAM. */
    __asm__ volatile("mcr p15, 0, %0, c7, c10, 0" :: "r"(0));  /* Clean D$ */
    __asm__ volatile("mcr p15, 0, %0, c7, c10, 4" :: "r"(0));  /* DSB */

    console_print("[MMU] Kernel L1 pre-populated\n");
}
