/*
 * fat32.h -- Minimal read-only FAT32 filesystem for the 3DS ARM9
 *
 * Reads the MBR and FAT32 BPB to locate files on the SD card.
 * Only 8.3 short filenames are supported.
 */

#ifndef FAT32_H
#define FAT32_H

#include "types9.h"

typedef struct {
    u32 partition_start_lba;   /* From MBR partition table           */
    u32 fat_start_lba;         /* First sector of the FAT            */
    u32 cluster_start_lba;     /* First sector of data cluster 2     */
    u32 sectors_per_cluster;   /* Cluster size in sectors            */
    u32 root_cluster;          /* Root directory first cluster        */
    u32 fat_size_sectors;      /* Size of one FAT copy in sectors    */
    u16 bytes_per_sector;      /* Almost always 512                  */
} fat32_fs_t;

/*
 * Read the MBR and FAT32 BPB from the SD card to fill 'fs'.
 * Locates the first FAT32 partition (type 0x0B or 0x0C).
 * Returns true on success.
 */
bool fat32_init(fat32_fs_t *fs);

/*
 * Search the directory starting at 'dir_cluster' for an entry whose
 * 8.3 name matches 'name83' (exactly 11 bytes: 8-char name + 3-char
 * extension, space-padded, uppercase).
 *
 * On success, writes the file's starting cluster and byte size to
 * *out_cluster and *out_size, and returns true.
 */
bool fat32_find_file(fat32_fs_t *fs, const char *name83,
                     u32 dir_cluster,
                     u32 *out_cluster, u32 *out_size);

/*
 * Read an entire file (given its starting cluster and byte size)
 * into the buffer at 'dest'.  Follows the FAT chain.
 * Returns true on success.
 */
bool fat32_read_file(fat32_fs_t *fs, u32 start_cluster,
                     u32 file_size, void *dest);

/* A contiguous run of a file's sectors on the card. */
typedef struct {
    u32 lba;
    u32 sectors;
} fat32_run_t;

/*
 * Resolve a file's cluster chain into at most max_runs contiguous runs
 * covering its first ceil(file_size / 512) sectors.  Returns false if the
 * chain is broken or needs more runs.
 */
bool fat32_file_runs(fat32_fs_t *fs, u32 start_cluster, u32 file_size,
                     fat32_run_t *runs, u32 max_runs, u32 *n_runs);

#endif /* FAT32_H */
