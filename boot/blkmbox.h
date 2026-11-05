/*
 * blkmbox.h — ARM11 <-> ARM9 block-read mailbox (shared by both builds)
 *
 * Only the ARM9 can drive the SD controller, so the ARM11's NAND emulation
 * asks the resident ARM9 block server for sectors of SD:/DISK.IMG.
 *
 * FCRAM layout (below kernel-managed RAM, which starts at 0x21000000; on the
 * ARM11 this range is mapped non-cacheable through the IO window, the ARM9
 * runs with its data cache off, so no cache maintenance is needed):
 *
 *   0x20C00000 - 0x20DFFFFF  read cache: 32 slots x 64 KB (ARM9 writes)
 *   0x20FFF000                this mailbox
 *
 * Protocol: the ARM11 fills op/sector/count/dst_pa, drains its write buffer,
 * then sets req_seq = ack_seq + 1.  The ARM9 sees req_seq != ack_seq, serves
 * the request, writes status, then ack_seq = req_seq.  The ARM9 refuses any
 * request outside the file or with dst_pa outside the read cache.
 */

#ifndef BLKMBOX_H
#define BLKMBOX_H

#define BLKMBOX_PA          0x20FFF000u
#define BLKCACHE_PA         0x20C00000u
#define BLKCACHE_SIZE       0x00200000u
#define BLKMBOX_MAGIC       0x394B4C42u     /* "BLK9": server ready      */
#define BLKMBOX_MAGIC_NOIMG 0x474D494Eu     /* "NIMG": no DISK.IMG found */
#define BLKMBOX_MAX_COUNT   128u            /* sectors per request (64 KB) */

#define BLK_OP_READ         1u

#define BLK_OK              0u
#define BLK_ERR_OP          1u              /* unsupported op (e.g. write) */
#define BLK_ERR_RANGE       2u              /* outside DISK.IMG            */
#define BLK_ERR_DST         3u              /* dst outside the read cache  */
#define BLK_ERR_IO          4u              /* SD read failed              */

typedef struct {
    volatile unsigned int magic;
    volatile unsigned int req_seq;
    volatile unsigned int ack_seq;
    volatile unsigned int op;
    volatile unsigned int sector;       /* 512-byte sector within DISK.IMG */
    volatile unsigned int count;        /* sectors, <= BLKMBOX_MAX_COUNT    */
    volatile unsigned int dst_pa;
    volatile unsigned int status;
    volatile unsigned int disk_sectors; /* DISK.IMG size / 512              */
    volatile unsigned int served;       /* requests completed (diagnostic) */
    volatile unsigned int extents;      /* FAT runs of DISK.IMG (diag)     */
} blkmbox_t;

#endif /* BLKMBOX_H */
