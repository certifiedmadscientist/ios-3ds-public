/*
 * mmu.h — ARMv6 MMU setup for iOS 3DS bootloader
 *
 * Configures short-descriptor page tables on the ARM11 MPCore to remap
 * memory so that the iPhone OS kernel sees S5L8900-compatible addresses
 * while running on 3DS hardware.
 */

#ifndef MMU_H
#define MMU_H

#include "types.h"

/* ── Memory layout near top of FCRAM ────────────────────────────────────
 *
 * 0x28000000              SVC stack top (grows down)
 * 0x27FF0000 - 0x27FF3FFF L1 page table (16KB, 16KB-aligned)
 * 0x27FE0000 - 0x27FEFFFF L2 page table pool (64KB)
 * 0x27FD0000 - 0x27FDFFFF Device tree buffer (64KB)
 * 0x27FC0000              ABT mode stack top (grows down)
 * 0x27FB0000              IRQ mode stack top (grows down)
 * ────────────────────────────────────────────────────────────────────── */

#define L1_TABLE_BASE       0x27FF0000
#define L1_TABLE_SIZE       0x4000      /* 16KB = 4096 entries x 4 bytes */

#define L2_POOL_BASE        0x27FE0000
#define L2_POOL_SIZE        0x10000     /* 64KB = up to 64 L2 tables */
#define L2_TABLE_SIZE       0x400       /* 1KB = 256 entries x 4 bytes */

#define DTREE_BUFFER_ADDR   0x27FD0000
#define DTREE_MAX_SIZE      0x10000     /* 64KB max device tree */

#define ABT_STACK_TOP       0x27FC0000
#define IRQ_STACK_TOP       0x27FB0000

/* ── ARMv6 L1 descriptor types ──────────────────────────────────────── */

#define L1_FAULT            0x00000000  /* Translation fault */
#define L1_COARSE           0x00000001  /* Points to L2 table */
#define L1_SECTION          0x00000002  /* 1MB section mapping */

/* ── Section descriptor flags ───────────────────────────────────────── */

#define SECTION_B           (1 << 2)            /* Bufferable */
#define SECTION_C           (1 << 3)            /* Cacheable */
#define SECTION_XN          (1 << 4)            /* Execute Never */
#define SECTION_DOMAIN(x)   (((x) & 0xF) << 5) /* Domain field */
#define SECTION_AP(x)       (((x) & 3) << 10)   /* Access permission */
#define SECTION_TEX(x)      (((x) & 7) << 12)  /* TEX bits */
#define SECTION_APX         (1 << 15)           /* AP extension */
#define SECTION_S           (1 << 16)           /* Shared */
#define SECTION_NG          (1 << 17)           /* Not global */

/* ── Common section types ───────────────────────────────────────────── */

/* Normal write-back write-allocate cached: TEX=001, C=1, B=1, AP=11 */
#define SECTION_NORMAL \
    (L1_SECTION | SECTION_TEX(1) | SECTION_C | SECTION_B | SECTION_AP(3))

/* Device (shareable, non-cacheable): TEX=000, C=0, B=1, AP=11 */
#define SECTION_DEVICE \
    (L1_SECTION | SECTION_B | SECTION_AP(3))

/* Normal non-cacheable: TEX=001, C=0, B=0, AP=11 */
#define SECTION_UNCACHED \
    (L1_SECTION | SECTION_TEX(1) | SECTION_AP(3))

/* ── L2 small page (4KB) descriptor ─────────────────────────────────── */

#define L2_FAULT            0x00000000
#define L2_SMALL_PAGE       0x00000002  /* 4KB page, XN=0 */
#define L2_PAGE_B           (1 << 2)
#define L2_PAGE_C           (1 << 3)
#define L2_PAGE_AP(x)       (((x) & 3) << 4)
#define L2_PAGE_TEX(x)      (((x) & 7) << 6)
#define L2_PAGE_APX         (1 << 9)
#define L2_PAGE_S           (1 << 10)
#define L2_PAGE_NG          (1 << 11)

/* Normal cached 4KB page: TEX=001, C=1, B=1, AP=11 */
#define L2_PAGE_NORMAL \
    (L2_SMALL_PAGE | L2_PAGE_TEX(1) | L2_PAGE_C | L2_PAGE_B | L2_PAGE_AP(3))

/* Device 4KB page: TEX=000, C=0, B=1, AP=11 */
#define L2_PAGE_DEVICE \
    (L2_SMALL_PAGE | L2_PAGE_B | L2_PAGE_AP(3))

/* ── Kernel L1 table pre-population ────────────────────────────────────
 *
 * Before jumping to the kernel we write entries into the kernel's own
 * L1 page table so that exception vectors and the kernel text/data
 * virtual range are mapped from the moment the kernel installs its
 * tables via TTBR.  The kernel will overwrite these once it is ready.
 * ──────────────────────────────────────────────────────────────────── */

/* Physical address of the kernel's L1 table (= topOfKernelData) */
#define KERN_L1_TABLE_BASE  0x23500000

/* L2 table for exception vectors, just past the kernel's 16KB L1 table */
#define KERN_L2_VEC_BASE    (KERN_L1_TABLE_BASE + L1_TABLE_SIZE) /* 0x22004000 */

/* ── Kernel framebuffer mapping addresses ──────────────────────────────
 * These kernel VAs let kernel code blit directly to the 3DS GPU FBs.
 * Placed at 0xBE100000+ (above user split at 0x80000000, below kernel
 * base at 0xC0000000) so they don't conflict with either. */
#define KERN_FB_TOP_VA      0xBE100000  /* → phys FB_TOP_ADDR */
#define KERN_FB_BOT_VA      0xBE200000  /* → phys FB_BOT_ADDR */
#define KERN_GPU_REG_VA     0xBE400000  /* → phys 0x10400000 */

/* ── Public API ─────────────────────────────────────────────────────── */

/* Set up page tables and enable the MMU.
 * fcram_size: total FCRAM in bytes (128 MB on Old 3DS, 256 MB on New).
 * Establishes all memory mappings for the HTL and enables caches. */
void mmu_init(u32 fcram_size);

/* Pre-populate the kernel's L1 page table at KERN_L1_TABLE_BASE with
 * mappings the kernel needs during early boot before it installs its
 * own (exception vectors at 0xFFFF0000 and kernel text/data at
 * 0xC0000000+).  Call just before jumping to the kernel.
 * fcram_size: total FCRAM in bytes (same value passed to mmu_init). */
void mmu_prepopulate_kernel_l1(u32 fcram_size);

/* Map a 1MB section. virt and phys must be 1MB-aligned.
 * flags should be one of the SECTION_* constants. */
void mmu_map_section(u32 virt, u32 phys, u32 flags);

/* Map a 4KB page. Allocates an L2 table if one does not exist
 * for the corresponding 1MB section. */
void mmu_map_page(u32 virt, u32 phys, u32 flags);

/* Remove the mapping for a 1MB section (sets entry to fault). */
void mmu_unmap(u32 virt);

#endif /* MMU_H */
