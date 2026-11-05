/*
 * main.c — C entry point for iOS 3DS bare-metal FIRM payload
 *
 * Initializes screens and debug console, then brings up the bootloader
 * subsystems: MMU (memory remapping), HTL (peripheral trap handler),
 * Apple device tree, and Mach-O loader infrastructure.
 */

#include "types.h"
#include "3ds_hw.h"
#include "screen.h"
#include "console.h"
#include "mmu.h"
#include "htl.h"
#include "dtree.h"
#include "loader.h"
#include "bootargs.h"
#include "periph/periph.h"
#include "iowin.h"
#include "display.h"
#include "periph/input.h"

/* Detect New 3DS vs Old 3DS via PDN clock register */
static bool is_new_3ds(void) {
    return (PDN_MPCORE_CLKCNT & 0x1) != 0;
}

/* Diagnostic: fill bottom screen with a BGR color (no deps) */
static void diag_fill(u8 b, u8 g, u8 r) {
    volatile u32 *reg = (volatile u32 *)io_va(0x10400568);
    volatile u8 *fb = (volatile u8 *)io_va(*reg);
    for (int i = 0; i < 230400; i += 3) {
        fb[i]   = b;
        fb[i+1] = g;
        fb[i+2] = r;
    }
}

NORETURN void main(void) {
    /* Stage 8: ORANGE — entered main() C code */
    diag_fill(0x00, 0xA5, 0xFF);

    /* ── 1. Screen and console ──────────────────────────────────────── */
    screen_init();

    /* Stage 9: PINK — screen_init() returned, write to runtime FB */
    diag_fill(0x80, 0x00, 0xFF);
    screen_bot_gradient();
    console_init();

    console_print("=====================================\n");
    console_print("  iOS 3DS bootloader\n");
    console_print("=====================================\n\n");

    console_print("Boot: FIRM chainload via Luma3DS\n");
    console_print("CPU:  ARM11 MPCore (ARMv6K)\n");

    u32 mem_size;
    if (is_new_3ds()) {
        console_print("Model: New Nintendo 3DS\n");
        mem_size = FCRAM_SIZE_N3DS;
    } else {
        console_print("Model: Old Nintendo 3DS\n");
        mem_size = FCRAM_SIZE_O3DS;
    }

    console_print("FCRAM: ");
    console_print_hex(mem_size);
    console_print("\n");

    console_print("SCTLR: ");
    {
        u32 sctlr;
        __asm__ volatile("mrc p15, 0, %0, c1, c0, 0" : "=r"(sctlr));
        console_print_hex(sctlr);
    }
    console_print("  MIDR: ");
    {
        u32 midr;
        __asm__ volatile("mrc p15, 0, %0, c0, c0, 0" : "=r"(midr));
        console_print_hex(midr);
    }
    console_print("\n\n");

    /* ── 2. MMU — remap memory for iPhone OS compatibility ──────────── */
    mmu_init(mem_size);
    console_print("\n");

    /* ── 3. HTL — install S5L8900 peripheral trap handler ───────────── */
    periph_init();
    htl_init();
    console_print("\n");

    /* ── 4. Boot command line ──────────────────────────────────────── */
#if ROOT_NAND
    static const char *boot_cmd = "debug=0x8 kextlog=0xfff io=0x6 cpus=1 serial=1 rd=disk0s1 -v";
#else
    static const char *boot_cmd = "debug=0x8 kextlog=0xfff io=0x6 cpus=1 serial=1 rd=md0 -v";
#endif

    /* ── 5. Wait for ARM9 to load kernel from SD ─────────────────────── */

    #define KERNEL_STAGING      0x24000000
    #define KERNEL_MAX_SIZE     0x00800000
    #define RAMDISK_STAGING     0x23000000
    #define RAMDISK_MAX_SIZE    0x00800000
    #define MAILBOX_ADDR        0x23FFFFE0
    #define MAILBOX_MAGIC       0x4B524E4C  /* "KRNL" */

    typedef struct {
        vu32 magic;
        vu32 kernel_addr;
        vu32 kernel_size;
        vu32 status;        /* 0=loading, 1=done, 0xFF=error */
        vu32 ramdisk_addr;
        vu32 ramdisk_size;
        vu32 reserved[2];   /* padding */
    } mailbox_t;

    volatile mailbox_t *mbox = (volatile mailbox_t *)MAILBOX_ADDR;

    u32 rd_addr = 0, rd_size = 0;
    console_print("[BOOT] Waiting for ARM9 SD loader...\n");

    /* Poll until ARM9 sets status to non-zero (done or error)
       or we time out.  status == 0 means "still loading". */
    u32 wait_count = 0;
    while (mbox->status == 0 && wait_count < 500000000) {
        wait_count++;
        /* Invalidate D-cache for mailbox so we see ARM9's writes.
           ARM9 has D-cache off — its writes go straight to RAM,
           but ARM11's cache holds a stale copy. */
        if ((wait_count & 0xFFF) == 0) {
            __asm__ volatile("mcr p15, 0, %0, c7, c6, 1" :: "r"(MAILBOX_ADDR));
            __asm__ volatile("mcr p15, 0, %0, c7, c10, 4" :: "r"(0));
        }
    }

    /* Invalidate mailbox cache lines and DMB before final read. */
    __asm__ volatile("mcr p15, 0, %0, c7, c6, 1" :: "r"(MAILBOX_ADDR));
    __asm__ volatile("mcr p15, 0, %0, c7, c10, 4" :: "r"(0));
    __asm__ volatile("mcr p15, 0, %0, c7, c10, 5" :: "r"(0));

    /* Always print mailbox state for debugging */
    console_print("[BOOT] Mailbox: magic=");
    console_print_hex(mbox->magic);
    console_print(" status=");
    console_print_hex(mbox->status);
    console_print("\n");
    console_print("  step=");
    console_print_hex(mbox->reserved[0]);
    console_print(" diag=");
    console_print_hex(mbox->reserved[1]);
    console_print(" ksize=");
    console_print_hex(mbox->kernel_size);
    console_print("\n");

    if (mbox->magic == MAILBOX_MAGIC && mbox->status == 1) {
        console_print("[BOOT] ARM9 loaded kernel from SD: ");
        console_print_hex(mbox->kernel_size);
        console_print(" bytes\n");
        if (mbox->ramdisk_size > 0) {
            console_print("[BOOT] Ramdisk loaded: ");
            console_print_hex(mbox->ramdisk_size);
            console_print(" bytes at ");
            console_print_hex(mbox->ramdisk_addr);
            console_print("\n");
        }
        rd_addr = mbox->ramdisk_addr;
        rd_size = mbox->ramdisk_size;
    } else if (mbox->status == 0) {
        /* Timeout — check if kernel was pre-loaded at staging */
        u32 kmagic = *(volatile u32 *)KERNEL_STAGING;
        if (kmagic == MH_MAGIC) {
            console_print("[BOOT] Kernelcache found pre-loaded\n");
        } else {
            console_print("[BOOT] ARM9 timed out, no kernel found\n");
            console_print("  (magic=0xA9A9A9A9 means ARM9 ran but SD failed)\n");
            goto halt;
        }
    } else {
        /* ARM9 reported an error */
        console_print("[BOOT] ARM9 SD error: status=0x");
        console_print_hex(mbox->status);
        console_print("\n");
        goto halt;
    }

    /* ── 5b. Apple Device Tree (needs ramdisk info from ARM9) ──────── */
    /* mdevinit() uses the RAMDisk address as a kernel virtual pointer,
       so convert phys → virt before storing in the device tree. */
    u32 rd_virt = rd_addr ? (rd_addr - BOOTARGS_PHYS_BASE + BOOTARGS_VIRT_BASE) : 0;
    size_t dtree_size = dtree_build_n45ap(
        (void *)DTREE_BUFFER_ADDR,
        DTREE_MAX_SIZE,
        mem_size,
        rd_virt,
        rd_size
    );
    if (!dtree_size)
        goto halt;
    console_print("\n");

    {
        const void *kdata = (const void *)KERNEL_STAGING;

        kernel_info_t kinfo;
        if (!loader_parse_macho(kdata, KERNEL_MAX_SIZE, &kinfo)) {
            console_print("[BOOT] FATAL: failed to parse kernelcache\n");
            goto halt;
        }

        console_print("[BOOT] Loading segments...\n");
        if (!loader_load_segments(kdata, KERNEL_MAX_SIZE)) {
            console_print("[BOOT] FATAL: failed to load segments\n");
            goto halt;
        }

        console_print("[BOOT] Kernel loaded\n");
        console_print("  Base:  ");
        console_print_hex(kinfo.load_base);
        console_print("\n  End:   ");
        console_print_hex(kinfo.load_end);
        console_print("\n  Entry: ");
        console_print_hex(kinfo.entry_point);
        console_print("\n");

        /* ── 6. Build boot_args struct ─────────────────────────────────── */
        boot_args_t *ba = bootargs_build(
            (void *)DTREE_BUFFER_ADDR, (u32)dtree_size,
            mem_size, boot_cmd
        );

        /* Pre-populate the kernel's L1 page table with exception vectors
         * and text/data section mappings so they are valid the moment the
         * kernel installs its TTBR. */
        mmu_prepopulate_kernel_l1(mem_size);

        /* ── Switch ABT/UND stacks to kernel VA space ──────────────── */
        /* After the kernel installs its TTBR, identity-mapped FCRAM
         * (0x27Fxxxxx) is gone.  The kern_htl stacks at 0xC07xxxxx
         * remain mapped because the kernel maps 0xC0000000+. */
        {
            extern u32 kern_htl_abt_stack_top;
            extern u32 kern_htl_und_stack_top;
            u32 new_abt_sp = (u32)&kern_htl_abt_stack_top;
            u32 new_und_sp = (u32)&kern_htl_und_stack_top;
            __asm__ volatile(
                "mrs r0, cpsr          \n"
                "bic r1, r0, #0x1F     \n"
                "orr r1, r1, #0x17     \n"  /* ABT mode */
                "msr cpsr_c, r1        \n"
                "mov sp, %0            \n"
                "bic r1, r0, #0x1F     \n"
                "orr r1, r1, #0x1B     \n"  /* UND mode */
                "msr cpsr_c, r1        \n"
                "mov sp, %1            \n"
                "msr cpsr_c, r0        \n"  /* restore original mode */
                :: "r"(new_abt_sp), "r"(new_und_sp)
                : "r0", "r1"
            );
            console_print("[BOOT] ABT/UND stacks -> kernel VA\n");
        }

        /* ── 6b. Draw iOS-style boot screen ────────────────────────────── */
        /* Black background with Apple logo position and "iOS 3DS" text.
           Draw a gradient + logo AFTER kernel is loaded so it's the last
           thing visible before XNU takes over the display. */
        screen_top_fill(0, 0, 0);  /* Clear to black */

        /* Draw a centered white Apple-ish logo (simple, 40x48 pixels) */
        {
            /* Draw an iOS-style gradient bar across the top */
            for (u32 x = 80; x < 320; x++) {
                u8 intensity = (u8)(((x - 80) * 255) / 240);
                for (u32 y = 100; y < 104; y++) {
                    screen_top_pixel(x, y, intensity, intensity, 255);
                }
            }

            /* "iOS" text - simple block letters, centered ~(170,110) */
            /* Letter 'i' - dot + stem */
            for (u32 y = 112; y < 116; y++)
                for (u32 x = 172; x < 176; x++)
                    screen_top_pixel(x, y, 255, 255, 255);
            for (u32 y = 120; y < 140; y++)
                for (u32 x = 172; x < 176; x++)
                    screen_top_pixel(x, y, 255, 255, 255);

            /* Letter 'O' - hollow rectangle */
            for (u32 y = 120; y < 140; y++) {
                for (u32 x = 180; x < 196; x++) {
                    if (y < 124 || y >= 136 || x < 184 || x >= 192)
                        screen_top_pixel(x, y, 255, 255, 255);
                }
            }

            /* Letter 'S' - three horizontal bars */
            for (u32 x = 200; x < 216; x++) {
                for (u32 y = 120; y < 124; y++)
                    screen_top_pixel(x, y, 255, 255, 255);
                for (u32 y = 128; y < 132; y++)
                    screen_top_pixel(x, y, 255, 255, 255);
                for (u32 y = 136; y < 140; y++)
                    screen_top_pixel(x, y, 255, 255, 255);
            }
            /* S left/right bars */
            for (u32 y = 120; y < 132; y++)
                for (u32 x = 200; x < 204; x++)
                    screen_top_pixel(x, y, 255, 255, 255);
            for (u32 y = 128; y < 140; y++)
                for (u32 x = 212; x < 216; x++)
                    screen_top_pixel(x, y, 255, 255, 255);

            /* "3DS" smaller text below */
            /* '3' */
            for (u32 x = 178; x < 190; x++) {
                for (u32 y = 148; y < 150; y++)
                    screen_top_pixel(x, y, 128, 180, 255);
                for (u32 y = 154; y < 156; y++)
                    screen_top_pixel(x, y, 128, 180, 255);
                for (u32 y = 160; y < 162; y++)
                    screen_top_pixel(x, y, 128, 180, 255);
            }
            for (u32 y = 148; y < 162; y++)
                for (u32 x = 188; x < 190; x++)
                    screen_top_pixel(x, y, 128, 180, 255);

            /* 'D' */
            for (u32 y = 148; y < 162; y++)
                for (u32 x = 194; x < 196; x++)
                    screen_top_pixel(x, y, 128, 180, 255);
            for (u32 y = 148; y < 162; y++)
                for (u32 x = 204; x < 206; x++)
                    screen_top_pixel(x, y, 128, 180, 255);
            for (u32 x = 194; x < 206; x++) {
                for (u32 y = 148; y < 150; y++)
                    screen_top_pixel(x, y, 128, 180, 255);
                for (u32 y = 160; y < 162; y++)
                    screen_top_pixel(x, y, 128, 180, 255);
            }

            /* 'S' */
            for (u32 x = 210; x < 222; x++) {
                for (u32 y = 148; y < 150; y++)
                    screen_top_pixel(x, y, 128, 180, 255);
                for (u32 y = 154; y < 156; y++)
                    screen_top_pixel(x, y, 128, 180, 255);
                for (u32 y = 160; y < 162; y++)
                    screen_top_pixel(x, y, 128, 180, 255);
            }
            for (u32 y = 148; y < 156; y++)
                for (u32 x = 210; x < 212; x++)
                    screen_top_pixel(x, y, 128, 180, 255);
            for (u32 y = 154; y < 162; y++)
                for (u32 x = 220; x < 222; x++)
                    screen_top_pixel(x, y, 128, 180, 255);
        }

        /* Patch after the boot screen is drawn so its log stays visible */
        loader_patch_kernel_vectors(&kinfo);

        /* Mach trap -10 (framebuffer) -> our display path.  Entry 10 of
         * mach_trap_table is {argc, fn}; the patched kernel's own cave
         * targets top-screen VRAM, ours draws the iPhone framebuffer. */
        {
            extern void kern_htl_fb_trap(void);
            volatile u32 *fn = (volatile u32 *)0xC01A5714;
            if (*fn == 0xC01A4D8C) {
                *fn = (u32)kern_htl_fb_trap;
                console_print("[DISP] trap -10 -> ");
                console_print_hex(*fn);
                console_print("\n");
            } else {
                console_print("[DISP] trap -10 unexpected ");
                console_print_hex(*fn);
                console_print(", left alone\n");
            }
        }
        {
            extern bool pm_patch_kernel(void);  /* Hold -> sleep (pm.c) */
            pm_patch_kernel();
        }
        display_boot_pattern();
        input_init();
        {
            extern void kern_htl_da_entry(void);
            console_print("[CHK] DA lit=");
            console_print_hex(*(volatile u32 *)0xC005F030);
            console_print(" img@");
            console_print_hex(kva2pa((u32)kern_htl_da_entry));
            console_print("\n[CHK] topFB=");
            console_print_hex(screen_top_fb_addr);
            console_print(" botFB=");
            console_print_hex(screen_bot_fb_addr);
            console_print("\n");
        }
        console_print("\n[BOOT] Jumping to kernel...\n\n");

        /* DEBUG: Keep GPU on FCRAM so console output remains visible.
         * TODO: switch to VRAM once kernel boot is working. */
        console_print("[BOOT] GPU stays on FCRAM (debug mode)\n");

        /* Enable ARM1176 cycle counter (ARMv6 PMU at c15, not ARMv7 c9).
         * The patched XNU _ml_get_timebase reads it as a fallback
         * before the S5L8900 timer is mapped by IOKit. */
        {
            u32 pmcr;
            __asm__ volatile("mrc p15, 0, %0, c15, c12, 0" : "=r"(pmcr));
            pmcr |= 1;  /* Enable cycle counter */
            __asm__ volatile("mcr p15, 0, %0, c15, c12, 0" :: "r"(pmcr));
        }

        {
            extern void rd_print_sums(u32 base);
            console_print("[BOOT] ramdisk sums (want 2601709E E1D3B6AA 0 EAA61AC0)\n");
            rd_print_sums(0x23000000u);
        }
        loader_jump_to_kernel(kinfo.entry_point, (void *)ba);
    }

    console_print("\nReady. Halting.\n");

halt:
    for (;;) {
        __asm__ volatile("wfi");
    }
}
