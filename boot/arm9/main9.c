/*
 * main9.c -- ARM9 SD card loader for iOS 3DS bootloader
 *
 * Initializes the SD card, reads the FAT32 filesystem, locates
 * /kernel.bin (8.3 name "KERNEL  BIN"), copies its contents to the
 * kernel staging area in FCRAM, and signals the ARM11 via a shared
 * mailbox in FCRAM.
 */

#include "types9.h"
#include "tmio.h"
#include "fat32.h"

void blkserv_reset_mailbox(void);
void blkserv_run(fat32_fs_t *fs);

/* ================================================================
 * Shared memory layout (FCRAM, visible to both ARM9 and ARM11)
 * ================================================================ */

#define KERNEL_STAGING      0x24000000u  /* 64 MB into FCRAM            */
#define KERNEL_MAX_SIZE     0x00800000u  /* 8 MB max kernelcache        */

#define RAMDISK_STAGING     0x23000000u  /* Below mailbox in FCRAM      */
#define RAMDISK_MAX_SIZE    0x00800000u  /* 8 MB max ramdisk            */

/* ARM9 -> ARM11 mailbox sits just below the staging area.
 * 0x23FFFFE0 + 32 bytes = 0x24000000. */
#define MAILBOX_ADDR        0x23FFFFE0u
#define MAILBOX_MAGIC       0x4B524E4Cu  /* "KRNL" (little-endian)      */

/* Mailbox status codes */
#define STATUS_LOADING      0x00u
#define STATUS_DONE         0x01u
#define STATUS_ERR_SD       0xFFu
#define STATUS_ERR_FS       0xFEu
#define STATUS_ERR_NOTFOUND 0xFDu
#define STATUS_ERR_TOOBIG   0xFCu
#define STATUS_ERR_READ     0xFBu

typedef struct {
    volatile u32 magic;        /* Set to MAILBOX_MAGIC when done      */
    volatile u32 kernel_addr;  /* Load address of the kernel          */
    volatile u32 kernel_size;  /* Byte size of the kernel file        */
    volatile u32 status;       /* STATUS_* code                       */
    volatile u32 ramdisk_addr; /* Load address of the ramdisk (0=none)*/
    volatile u32 ramdisk_size; /* Byte size of the ramdisk file       */
    volatile u32 reserved[2];  /* Padding to 32 bytes                 */
} mailbox_t;

#define MAILBOX ((mailbox_t *)MAILBOX_ADDR)

/* Drain the ARM9 write buffer so FCRAM writes are visible. */
static void dsb9(void)
{
    __asm__ volatile("mcr p15, 0, %0, c7, c10, 4" :: "r"(0));
}


/* ================================================================
 * ARM9 entry point
 * ================================================================ */

NORETURN void arm9_main(void)
{
    mailbox_t *mb = MAILBOX;
    fat32_fs_t fs;
    u32 start_cluster;
    u32 file_size;

    /* Mark loading in progress — use reserved[0] as step counter
       so ARM11 can see exactly where ARM9 reaches. */
    mb->status       = STATUS_LOADING;
    mb->magic        = 0xA9000000;
    mb->ramdisk_addr = 0;
    mb->ramdisk_size = 0;
    mb->reserved[0]  = 0;
    dsb9();
    blkserv_reset_mailbox();

    /* BLACK — ARM9 starting */

    /* Step 1: TMIO init — DARK RED */
    mb->reserved[0] = 1; dsb9();
    if (!tmio_init(false)) {
        mb->status = STATUS_ERR_SD;
        mb->reserved[0] = 0xE1;
        dsb9();
        goto halt;
    }

    /* Step 2: FAT32 init — DARK GREEN */
    mb->reserved[0] = 2; dsb9();
    if (!fat32_init(&fs)) {
        mb->status = STATUS_ERR_FS;
        mb->reserved[0] = 0xE2;
        mb->reserved[1] = tmio_last_istat();
        dsb9();
        goto halt;
    }

    /* Step 3: Find /kernel.bin — DARK BLUE */
    mb->reserved[0] = 3; dsb9();

    /* ---- 3. Find /kernel.bin ---- */
    /*  8.3 short name: "KERNEL  BIN"
     *  (6-char name "KERNEL" + 2 spaces + 3-char ext "BIN") */
    if (!fat32_find_file(&fs, "KERNEL  BIN", fs.root_cluster,
                         &start_cluster, &file_size)) {
        mb->status = STATUS_ERR_NOTFOUND;
        dsb9();
        goto halt;
    }

    if (file_size > KERNEL_MAX_SIZE) {
        mb->status = STATUS_ERR_TOOBIG;
        dsb9();
        goto halt;
    }

    /* Step 4: Read kernel — CYAN */
    mb->reserved[0] = 4; dsb9();
    if (!fat32_read_file(&fs, start_cluster, file_size,
                         (void *)KERNEL_STAGING)) {
        mb->status = STATUS_ERR_READ;
        dsb9();
        goto halt;
    }

    /* Step 5: Read ramdisk — WHITE */
    mb->reserved[0] = 5; dsb9();

    /* ---- 4b. Try to load /ramdisk.dmg ---- */
    /* 8.3 short name: "RAMDISK DMG" (7-char name + 1 space + 3-char ext) */
    u32 rd_cluster, rd_size;
    if (fat32_find_file(&fs, "RAMDISK DMG", fs.root_cluster,
                        &rd_cluster, &rd_size)) {
        if (rd_size <= RAMDISK_MAX_SIZE) {
            if (fat32_read_file(&fs, rd_cluster, rd_size,
                                (void *)RAMDISK_STAGING)) {
                mb->ramdisk_addr = RAMDISK_STAGING;
                mb->ramdisk_size = rd_size;
            }
        }
    }

    /* ---- 5. Signal ARM11 ---- */
    mb->kernel_size = file_size;
    mb->kernel_addr = KERNEL_STAGING;
    mb->magic       = MAILBOX_MAGIC;
    mb->status      = STATUS_DONE;
    dsb9();

    /* Stay resident and serve the emulated NAND from SD:/DISK.IMG
     * (returns only if there is no usable image). */
    blkserv_run(&fs);

halt:
    /* Park the ARM9 forever. */
    for (;;) {
        __asm__ volatile("mcr p15, 0, %0, c7, c0, 4" :: "r"(0));
    }
}
