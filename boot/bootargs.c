/*
 * bootargs.c — Build the XNU boot_args struct for iOS 3DS bootloader
 *
 * Populates a boot_args_t in identity-mapped FCRAM so the kernel can
 * read it at entry (through our page tables) and the values remain
 * valid after the kernel switches to its own page tables.
 */

#include "bootargs.h"
#include "3ds_hw.h"
#include "console.h"

/* ── Helpers ────────────────────────────────────────────────────────── */

static void ba_mem_zero(void *dst, u32 n) {
    u8 *d = (u8 *)dst;
    for (u32 i = 0; i < n; i++)
        d[i] = 0;
}

static void ba_str_copy(char *dst, const char *src, u32 max) {
    u32 i;
    for (i = 0; i < max - 1 && src[i]; i++)
        dst[i] = src[i];
    for (; i < max; i++)
        dst[i] = '\0';
}

/* ── Build boot_args ───────────────────────────────────────────────── */

boot_args_t *bootargs_build(void *device_tree, u32 device_tree_len,
                            u32 mem_size, const char *cmd_line)
{
    boot_args_t *ba = (boot_args_t *)BOOTARGS_ADDR;

    /* Zero the entire struct first */
    ba_mem_zero(ba, sizeof(boot_args_t));

    /* Header */
    ba->revision = 1;
    ba->version  = 4;

    /* Memory layout — the kernel uses these to build its page tables:
     *   virtBase -> physBase section mapping in its L1 table
     *   topOfKernelData = where the L1 table itself lives (TTBR) */
    ba->virtBase        = BOOTARGS_VIRT_BASE;   /* 0xC0000000 */
    ba->physBase        = BOOTARGS_PHYS_BASE;   /* 0x21000000 */
    ba->topOfKernelData = BOOTARGS_TOP_OF_DATA; /* 0x22000000 */

    /* memSize = usable FCRAM from physBase to end of FCRAM.
     * The kernel maps virtBase → physBase for memSize bytes, and we must
     * not map past the end of real FCRAM. */
    ba->memSize = (FCRAM_BASE + mem_size) - BOOTARGS_PHYS_BASE;

    /* Display — point at a scratch framebuffer in low FCRAM.
     * iPhone OS 1.0 expects non-zero dimensions; with zeroes
     * the PE_init_iokit console loop underflows and hangs. */
    ba->Video.v_baseAddr = BOOTARGS_VIDEO_BASE;
    ba->Video.v_display  = 1;
    ba->Video.v_rowBytes = LCD_STRIDE;
    ba->Video.v_width    = LCD_W;
    ba->Video.v_height   = LCD_H;
    ba->Video.v_depth    = 32;

    /* Machine type — 0 for generic ARM / iPod touch */
    ba->machineType = 0;

    /* Device tree — convert physical address to kernel virtual address */
    ba->deviceTreeP      = (void *)((u32)device_tree - BOOTARGS_PHYS_BASE + BOOTARGS_VIRT_BASE);
    ba->deviceTreeLength = device_tree_len;

    /* Command line */
    ba_str_copy(ba->CommandLine, cmd_line, sizeof(ba->CommandLine));

    /* ── Debug output ──────────────────────────────────────────────── */

    console_print("[BOOTARGS] Built at ");
    console_print_hex(BOOTARGS_ADDR);
    console_print("\n");
    console_print("  revision:         ");
    console_print_hex(ba->revision);
    console_print("\n");
    console_print("  virtBase:         ");
    console_print_hex(ba->virtBase);
    console_print("\n");
    console_print("  physBase:         ");
    console_print_hex(ba->physBase);
    console_print("\n");
    console_print("  memSize:          ");
    console_print_hex(ba->memSize);
    console_print("\n");
    console_print("  topOfKernelData:  ");
    console_print_hex(ba->topOfKernelData);
    console_print("\n");
    console_print("  deviceTreeP:      ");
    console_print_hex((u32)ba->deviceTreeP);
    console_print("\n");
    console_print("  deviceTreeLength: ");
    console_print_hex(ba->deviceTreeLength);
    console_print("\n");

    return ba;
}
