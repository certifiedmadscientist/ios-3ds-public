/*
 * nand_hal_3ds.c — NAND emulation environment on the 3DS.
 *
 * Disk pages come from the resident ARM9 block server (blkmbox.h).  Kernel
 * RAM is reached through the kernel's cacheable linear map with DMA-style
 * cache maintenance (see lines()).
 */

#include "nand_hal.h"
#include "vic.h"
#include "../blkmbox.h"
#include "../iowin.h"
#include "../console.h"

#define MBOX ((blkmbox_t *)io_va(BLKMBOX_PA))

/* Spin budget for one ARM9 request (~64 KB from SD takes a few ms). */
#define BLK_SPIN 200000000u

static void dsb(void) {
    __asm__ volatile("mcr p15, 0, %0, c7, c10, 4" :: "r"(0) : "memory");
}

u8 *nhal_slot(u32 n) {
    return (u8 *)io_va(BLKCACHE_PA + (n % NHAL_SLOTS) * NHAL_SLOT_BYTES);
}

/* The ARM9 publishes BLK9 (or NIMG) once DISK.IMG's runs are resolved;
 * wait for that if the kernel probes the NAND before it's done. */
static bool mbox_ready(void) {
    blkmbox_t *m = MBOX;
    for (u32 spin = 0; m->magic == 0 && spin < BLK_SPIN; spin++)
        ;
    if (m->magic == BLKMBOX_MAGIC)
        return true;
    static bool logged;
    if (!logged) {
        logged = true;
        nhal_log("[NAND] no ARM9 block server, magic ", m->magic);
    }
    return false;
}

u32 nhal_disk_sectors(void) {
    return mbox_ready() ? MBOX->disk_sectors : 0;
}

bool nhal_fill(u32 n, u32 sector, u32 count) {
    blkmbox_t *m = MBOX;
    if (!mbox_ready() || count == 0 || count > BLKMBOX_MAX_COUNT)
        return false;

    u32 seq = m->ack_seq + 1;
    m->op = BLK_OP_READ;
    m->sector = sector;
    m->count = count;
    m->dst_pa = BLKCACHE_PA + (n % NHAL_SLOTS) * NHAL_SLOT_BYTES;
    m->status = 0xFFFFFFFF;
    dsb();
    m->req_seq = seq;
    dsb();

    for (u32 spin = 0; spin < BLK_SPIN; spin++) {
        if (m->ack_seq == seq) {
            if (m->status != BLK_OK)
                nhal_log("[NAND] ARM9 block error ", m->status);
            return m->status == BLK_OK;
        }
    }
    nhal_log("[NAND] ARM9 block server timeout, sector ", sector);
    return false;
}

/* Write overlay: 16 MB of FCRAM at PA 0x22000000-0x22FFFFFF, between our
 * image (0x21800000-0x21BFFFFF) and the ramdisk staging area (0x23000000,
 * ROOT=md only), below the kernel's avail_start (~0x2362F000) so the VM
 * never hands it out.  The kernel's static linear map reaches it at VA
 * 0xC1000000 in every address space; only our CPU code touches it, so the
 * cacheable mapping needs no maintenance.  7680 x 2112 bytes = 15.5 MB. */
#define OVL_PA  0x22000000u
#define OVL_VA  (OVL_PA - 0x21000000u + 0xC0000000u)

u8 *nhal_overlay(void) {
    return (u8 *)OVL_VA;
}

static bool kernel_ram(u32 pa, u32 len) {
    return pa >= 0x21000000u && len <= 0x07000000u &&
           pa - 0x21000000u <= 0x07000000u - len;
}

/* Everything we touch through here is memory the kernel shares with a DMA
 * device (ADM command block and data sections, PL080 descriptors and
 * buffers), so access it the way the device would: RAM, not the cache.
 * Clean+invalidate the lines before reading (anything dirty is the
 * kernel's newest data; anything clean may be a stale copy of ours) and
 * after writing, so no cached copy outlives what the device would see. */
static void lines(u32 va, u32 len) {
    for (u32 a = va & ~31u; a < va + len; a += 32)
        __asm__ volatile("mcr p15, 0, %0, c7, c14, 1" :: "r"(a) : "memory");
    dsb();
}

bool nhal_phys_read(u32 pa, void *dst, u32 len) {
    if (!kernel_ram(pa, len) || len == 0)
        return false;
    lines(io_va(pa), len);
    const volatile u8 *s = (const volatile u8 *)io_va(pa);
    u8 *d = dst;
    for (u32 i = 0; i < len; i++)
        d[i] = s[i];
    return true;
}

bool nhal_phys_write(u32 pa, const void *src, u32 len) {
    if (!kernel_ram(pa, len) || len == 0)
        return false;
    volatile u8 *d = (volatile u8 *)io_va(pa);
    const u8 *s = src;
    for (u32 i = 0; i < len; i++)
        d[i] = s[i];
    lines((u32)d, len);
    return true;
}

void nhal_irq(int line, bool level) {
    vic_set_irq(line, level);
}

void nhal_log(const char *msg, u32 val) {
    console_print(msg);
    console_print_hex(val);
    console_print("\n");
}

void nhal_puts(const char *line) {
    console_print(line);
    console_print("\n");
}

u32 nhal_ticks(void) {
    extern u32 kern_irq_taken;
    return kern_irq_taken;
}
