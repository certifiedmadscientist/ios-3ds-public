/*
 * bootargs.h — XNU boot_args struct for iOS 3DS bootloader
 *
 * The iPhone OS 1.x kernel expects r0 to point to a boot_args struct
 * at entry, NOT the device tree directly.  The kernel immediately reads
 * fields from this struct to set up its initial page tables:
 *
 *   ldr  r8, [r0, #8]     ; physBase
 *   ldr  r9, [r0, #4]     ; virtBase
 *   ldr  sl, [r0, #12]    ; memSize
 *   ldr  r4, [r0, #16]    ; topOfKernelData -> TTBR0/TTBR1
 *
 * Getting these wrong causes an immediate crash because the kernel
 * writes page table entries to a garbage address and installs it as
 * the translation table base.
 */

#ifndef BOOTARGS_H
#define BOOTARGS_H

#include "types.h"

/* ── Boot video descriptor (display info for the kernel) ───────────── */

typedef struct {
    u32 v_baseAddr;     /* framebuffer physical address */
    u32 v_display;      /* display code */
    u32 v_rowBytes;     /* bytes per row */
    u32 v_width;        /* pixels wide */
    u32 v_height;       /* pixels tall */
    u32 v_depth;        /* bits per pixel */
} PACKED boot_video_t;

/* ── Boot arguments struct (iPhone OS 1.x / XNU ARM) ──────────────── */

typedef struct {
    u16 revision;           /* +0x00: boot_args revision (1) */
    u16 version;            /* +0x02: boot_args version  (1) */
    u32 virtBase;           /* +0x04: kernel virtual base (0xC0000000) */
    u32 physBase;           /* +0x08: physical base of kernel DRAM */
    u32 memSize;            /* +0x0C: physical memory size in bytes */
    u32 topOfKernelData;    /* +0x10: end of kernel data = page table base */
    boot_video_t Video;     /* +0x14: inline Boot_Video (24 bytes) */
    u32 machineType;        /* +0x2C: Apple machine type (0 = iPod) */
    void *deviceTreeP;      /* +0x30: pointer to Apple device tree */
    u32 deviceTreeLength;   /* +0x34: device tree size in bytes */
    char CommandLine[256];  /* +0x38: kernel boot arguments string */
} PACKED boot_args_t;

/* ── Memory placement ──────────────────────────────────────────────── */

/*
 * Boot args struct lives in identity-mapped FCRAM (virt == phys), below
 * the IRQ stack area.  Memory map near top of FCRAM:
 *
 *   0x27FF0000  L1 page table
 *   0x27FE0000  L2 pool
 *   0x27FD0000  Device tree
 *   0x27FC0000  ABT stack top
 *   0x27FB0000  IRQ stack top
 *   0x27FA0000  boot_args struct  <-- here
 */
#define BOOTARGS_ADDR           0x27FA0000

/*
 * Physical base of kernel memory in real FCRAM.
 * Our MMU maps virt 0xC0000000 -> phys 0x21000000.
 * The Mach-O loader copies the kernel to virtual 0xC0000000+,
 * which writes to physical FCRAM at 0x21000000+.
 */
#define BOOTARGS_PHYS_BASE      0x21000000

/* Kernel virtual base — the Mach-O is linked at this address. */
#define BOOTARGS_VIRT_BASE      0xC0000000

/*
 * Page table area for the kernel.  Must be:
 *   1. 16KB-aligned (for ARMv6 L1 table)
 *   2. In identity-mapped FCRAM (virt == phys, so TTBR works)
 *   3. Past all loaded kernel data
 *
 * The kernel ends around physical 0x215C0000 (6MB from 0x21000000).
 * The ramdisk lives at 0x23000000 (up to 8MB).  topOfKernelData must
 * be past everything wired: kernel + ramdisk.  0x23500000 leaves a
 * gap after the ramdisk, and the L1 table is 16KB-aligned.
 */
#define BOOTARGS_TOP_OF_DATA    0x23500000

/* iPhone framebuffer (boot_args Video): 320x480 32bpp BGRA, below
 * physBase so the kernel never allocates it.  display.c mirrors it to
 * the 3DS bottom screen. */
#define BOOTARGS_VIDEO_BASE     0x20800000

/* Panel geometry handed to the kernel (boot_args Video and the CLCD window
 * iBoot leaves programmed): 320 x LCD_H, 32bpp BGRA.  Build with
 * LCD=iphone for the real 480 lines. */
#ifndef LCD_H
#define LCD_H                   240
#endif
#define LCD_W                   320
#define LCD_STRIDE              (LCD_W * 4)

/* Device-tree /vram: graphics memory for the CLCD framebuffer and
 * CoreSurface (4 MB, ending where the ARM9 block cache starts). */
#define DT_VRAM_PA                 0x20800000
#define DT_VRAM_SIZE            0x00400000

/* ── API ───────────────────────────────────────────────────────────── */

/*
 * Build a boot_args struct at BOOTARGS_ADDR.
 *
 * device_tree     — pointer to the Apple device tree in memory
 * device_tree_len — size of the device tree in bytes
 * mem_size        — FCRAM size (128MB on Old 3DS, 256MB on New 3DS)
 * cmd_line        — kernel boot arguments string
 *
 * Returns pointer to the populated boot_args_t.
 */
boot_args_t *bootargs_build(void *device_tree, u32 device_tree_len,
                            u32 mem_size, const char *cmd_line);

#endif /* BOOTARGS_H */
