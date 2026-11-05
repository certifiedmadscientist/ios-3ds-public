/*
 * blkserv.c -- resident ARM9 block server for the emulated NAND
 *
 * After the kernel and ramdisk are loaded, the ARM9 stays in this loop and
 * serves read requests for SD:/DISK.IMG from the ARM11 (see ../blkmbox.h).
 *
 * Safety: read-only (only BLK_OP_READ exists; tmio.c has no write commands),
 * SD port only (tmio.c's port guard), every sector is resolved through the
 * file's own cluster runs so nothing outside DISK.IMG is ever read, and
 * data is only written into the ARM11 read-cache window.
 */

#include "types9.h"
#include "tmio.h"
#include "fat32.h"
#include "../blkmbox.h"

#define MAX_RUNS 4096u

static fat32_run_t runs[MAX_RUNS];
static u32 n_runs;
static u32 disk_sectors;

static void dsb9(void)
{
    __asm__ volatile("mcr p15, 0, %0, c7, c10, 4" :: "r"(0) : "memory");
}

void blkserv_reset_mailbox(void)
{
    blkmbox_t *m = (blkmbox_t *)BLKMBOX_PA;
    m->magic = 0;
    m->req_seq = 0;
    m->ack_seq = 0;
    m->served = 0;
    dsb9();
}

/* Read `count` sectors of the image starting at `sector` into dst. */
static bool read_image(u32 sector, u32 count, u8 *dst)
{
    u32 base = 0;
    for (u32 r = 0; r < n_runs && count; r++) {
        u32 len = runs[r].sectors;
        if (sector < base + len) {
            u32 off = sector - base;
            u32 n = len - off < count ? len - off : count;
            if (!tmio_read_sectors(runs[r].lba + off, n, dst))
                return false;
            dst += n * 512;
            sector += n;
            count -= n;
        }
        base += len;
    }
    return count == 0;
}

static u32 serve(blkmbox_t *m)
{
    u32 op = m->op, sector = m->sector, count = m->count, dst = m->dst_pa;

    if (op != BLK_OP_READ)
        return BLK_ERR_OP;
    if (count == 0 || count > BLKMBOX_MAX_COUNT ||
        sector >= disk_sectors || count > disk_sectors - sector)
        return BLK_ERR_RANGE;
    if (dst < BLKCACHE_PA || dst - BLKCACHE_PA > BLKCACHE_SIZE - count * 512)
        return BLK_ERR_DST;
    return read_image(sector, count, (u8 *)dst) ? BLK_OK : BLK_ERR_IO;
}

/* Never returns once DISK.IMG is found; returns if there is no image. */
void blkserv_run(fat32_fs_t *fs)
{
    blkmbox_t *m = (blkmbox_t *)BLKMBOX_PA;
    u32 cluster, size;

    if (!fat32_find_file(fs, "DISK    IMG", fs->root_cluster, &cluster, &size) ||
        !fat32_file_runs(fs, cluster, size, runs, MAX_RUNS, &n_runs)) {
        m->magic = BLKMBOX_MAGIC_NOIMG;
        dsb9();
        return;
    }

    disk_sectors = size / 512;
    m->disk_sectors = disk_sectors;
    m->extents = n_runs;
    m->ack_seq = m->req_seq;
    dsb9();
    m->magic = BLKMBOX_MAGIC;
    dsb9();

    for (;;) {
        u32 seq = m->req_seq;
        if (seq == m->ack_seq)
            continue;
        m->status = serve(m);
        m->served++;
        dsb9();
        m->ack_seq = seq;
        dsb9();
    }
}
