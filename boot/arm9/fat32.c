/*
 * fat32.c -- Minimal read-only FAT32 filesystem for the 3DS ARM9
 *
 * Parses the MBR and FAT32 BPB, navigates directories using 8.3
 * short names, follows FAT chains, and reads file data from the
 * SD card.  No long-filename (LFN) support.
 */

#include "fat32.h"
#include "tmio.h"

/* ================================================================
 * Internal buffer -- used for MBR, BPB, FAT table, and directory
 * sector reads.  File data is read directly to the caller's buffer.
 * ================================================================ */

static u8 sector_buf[512] ALIGNED(4);

/* ================================================================
 * Little-endian helpers (ARM9 is LE, but be explicit)
 * ================================================================ */

static u16 read_u16_le(const u8 *p)
{
    return (u16)(p[0] | (p[1] << 8));
}

static u32 read_u32_le(const u8 *p)
{
    return (u32)(p[0] | (p[1] << 8) | (p[2] << 16) | (p[3] << 24));
}

/* ================================================================
 * Cluster arithmetic
 * ================================================================ */

static u32 cluster_to_lba(const fat32_fs_t *fs, u32 cluster)
{
    return fs->cluster_start_lba +
           (cluster - 2) * fs->sectors_per_cluster;
}

/*
 * Follow the FAT chain: given a cluster number, return the next
 * cluster in the chain.  Returns 0 if end-of-chain or error.
 */
static u32 fat32_next_cluster(fat32_fs_t *fs, u32 cluster)
{
    u32 fat_byte_offset = cluster * 4;
    u32 fat_sector = fs->fat_start_lba +
                     (fat_byte_offset / fs->bytes_per_sector);
    u32 entry_offset = fat_byte_offset % fs->bytes_per_sector;

    /* Consecutive clusters share a FAT sector: keep the last one. */
    static u8  fat_buf[512] ALIGNED(4);
    static u32 fat_buf_sector = 0xFFFFFFFFu;
    if (fat_sector != fat_buf_sector) {
        if (!tmio_read_sectors(fat_sector, 1, fat_buf)) {
            fat_buf_sector = 0xFFFFFFFFu;
            return 0;
        }
        fat_buf_sector = fat_sector;
    }

    u32 next = read_u32_le(&fat_buf[entry_offset]) & 0x0FFFFFFFu;
    if (next >= 0x0FFFFFF8u)
        return 0;   /* End of chain marker */
    return next;
}

/* ================================================================
 * Filesystem initialization
 * ================================================================ */

bool fat32_init(fat32_fs_t *fs)
{
    /* ---- Read MBR (sector 0) ---- */
    if (!tmio_read_sectors(0, 1, sector_buf))
        return false;

    /* Validate MBR boot signature. */
    if (sector_buf[510] != 0x55 || sector_buf[511] != 0xAA)
        return false;

    /* Scan the four partition table entries (offset 0x1BE, 16 bytes
       each) for the first FAT32 partition. */
    bool found = false;
    for (int i = 0; i < 4; i++) {
        const u8 *entry = &sector_buf[0x1BE + i * 16];
        u8 ptype = entry[4];
        if (ptype == 0x0B || ptype == 0x0C) {
            fs->partition_start_lba = read_u32_le(&entry[8]);
            found = true;
            break;
        }
    }
    if (!found)
        return false;

    /* ---- Read FAT32 BPB (first sector of the partition) ---- */
    if (!tmio_read_sectors(fs->partition_start_lba, 1, sector_buf))
        return false;

    fs->bytes_per_sector   = read_u16_le(&sector_buf[0x0B]);
    fs->sectors_per_cluster = sector_buf[0x0D];

    u16 reserved_sectors   = read_u16_le(&sector_buf[0x0E]);
    u8  num_fats           = sector_buf[0x10];

    fs->fat_size_sectors   = read_u32_le(&sector_buf[0x24]);
    fs->root_cluster       = read_u32_le(&sector_buf[0x2C]);

    /* Basic sanity checks. */
    if (fs->bytes_per_sector == 0 || fs->sectors_per_cluster == 0)
        return false;
    if (fs->bytes_per_sector != 512)
        return false;   /* We only handle 512-byte sectors. */
    if (num_fats == 0 || fs->fat_size_sectors == 0)
        return false;

    fs->fat_start_lba     = fs->partition_start_lba + reserved_sectors;
    fs->cluster_start_lba = fs->fat_start_lba +
                            ((u32)num_fats * fs->fat_size_sectors);

    return true;
}

/* ================================================================
 * Directory search (8.3 short names only)
 * ================================================================ */

/* Compare an 11-byte 8.3 directory entry name against 'name83'. */
static bool name_match(const u8 *entry, const char *name83)
{
    for (int i = 0; i < 11; i++) {
        if (entry[i] != (u8)name83[i])
            return false;
    }
    return true;
}

bool fat32_find_file(fat32_fs_t *fs, const char *name83,
                     u32 dir_cluster,
                     u32 *out_cluster, u32 *out_size)
{
    u32 cluster = dir_cluster;

    while (cluster != 0 && cluster < 0x0FFFFFF8u) {
        u32 lba = cluster_to_lba(fs, cluster);

        for (u32 s = 0; s < fs->sectors_per_cluster; s++) {
            if (!tmio_read_sectors(lba + s, 1, sector_buf))
                return false;

            /* 16 directory entries per 512-byte sector (32 bytes each). */
            for (u32 e = 0; e < 16; e++) {
                const u8 *ent = &sector_buf[e * 32];

                /* 0x00 = end of directory. */
                if (ent[0] == 0x00)
                    return false;

                /* 0xE5 = deleted entry -- skip. */
                if (ent[0] == 0xE5)
                    continue;

                /* Attribute 0x0F = LFN entry -- skip. */
                if (ent[0x0B] == 0x0F)
                    continue;

                if (name_match(ent, name83)) {
                    u32 cluster_hi = read_u16_le(&ent[0x14]);
                    u32 cluster_lo = read_u16_le(&ent[0x1A]);
                    *out_cluster = (cluster_hi << 16) | cluster_lo;
                    *out_size    = read_u32_le(&ent[0x1C]);
                    return true;
                }
            }
        }

        cluster = fat32_next_cluster(fs, cluster);
    }

    return false;
}

/* ================================================================
 * File reading
 * ================================================================ */

bool fat32_read_file(fat32_fs_t *fs, u32 start_cluster,
                     u32 file_size, void *dest)
{
    u32 cluster = start_cluster;
    u32 remaining = file_size;
    u8  *dst = (u8 *)dest;
    u32 cluster_bytes = fs->sectors_per_cluster * fs->bytes_per_sector;

    while (remaining > 0 && cluster != 0 && cluster < 0x0FFFFFF8u) {
        u32 lba = cluster_to_lba(fs, cluster);

        /* Bytes to read from this cluster. */
        u32 to_read = remaining;
        if (to_read > cluster_bytes)
            to_read = cluster_bytes;

        /* Read full sectors covering 'to_read' bytes. */
        u32 sectors = (to_read + fs->bytes_per_sector - 1) /
                       fs->bytes_per_sector;
        if (!tmio_read_sectors(lba, sectors, dst))
            return false;

        dst       += to_read;
        remaining -= to_read;

        if (remaining > 0)
            cluster = fat32_next_cluster(fs, cluster);
    }

    return (remaining == 0);
}

/* ================================================================
 * File extents
 * ================================================================ */

bool fat32_file_runs(fat32_fs_t *fs, u32 start_cluster, u32 file_size,
                     fat32_run_t *runs, u32 max_runs, u32 *n_runs)
{
    u32 cluster = start_cluster;
    u32 spc = fs->sectors_per_cluster;
    u32 need = (file_size + fs->bytes_per_sector - 1) / fs->bytes_per_sector;
    u32 n = 0, have = 0;

    while (have < need) {
        if (cluster < 2 || cluster >= 0x0FFFFFF8u)
            return false;               /* chain shorter than the file */
        u32 lba = cluster_to_lba(fs, cluster);
        u32 take = need - have < spc ? need - have : spc;

        if (n && runs[n - 1].lba + runs[n - 1].sectors == lba) {
            runs[n - 1].sectors += take;
        } else {
            if (n == max_runs)
                return false;           /* too fragmented */
            runs[n].lba = lba;
            runs[n].sectors = take;
            n++;
        }
        have += take;
        if (have < need)
            cluster = fat32_next_cluster(fs, cluster);
    }
    *n_runs = n;
    return true;
}
