/*
 * nand.h — S5L8900 NAND subsystem emulation (iPod touch 1G)
 *
 *   0x38800000  ADM  (flash command processor)   IRQ 37
 *   0x38A00000  FMC  (flash-controller0, FIFO at +0x80)  IRQ 20
 *   0x38F00000  RS-ECC                           IRQ 43
 *
 * The emulated NAND is 8 banks x 524288 pages of 2048 + 64 bytes, laid out
 * the way qemu-ios's touch-1G NAND generator (devos50/qemu-ios-generate-nand,
 * branch main) writes it.  Metadata pages (FIL ID, BBT, VFL and FTL context,
 * FTL map) are synthesized; data pages come from a flat disk image on the SD
 * card in which LBA n (2048 bytes) is FTL virtual page 206848 + n.
 */

#ifndef NAND_H
#define NAND_H

#include "../types.h"

#define NAND_ADM_BASE   0x38800000u
#define NAND_FMC_BASE   0x38A00000u
#define NAND_ECC_BASE   0x38F00000u
#define NAND_FIFO_PA    (NAND_FMC_BASE + 0x80u)

#define NAND_IRQ_ADM    37
#define NAND_IRQ_FMC    20
#define NAND_IRQ_ECC    43

#define NAND_PAGE_BYTES   2048u
#define NAND_SPARE_BYTES  64u
#define NAND_DATA_VPN0    206848u     /* FTL virtual page of disk LBA 0 */

void nand_init(void);

u32  nand_fmc_read(u32 off);
void nand_fmc_write(u32 off, u32 val);
u32  nand_ecc_read(u32 off);
void nand_ecc_write(u32 off, u32 val);
u32  nand_adm_read(u32 off);
void nand_adm_write(u32 off, u32 val);

/* FIFO port as seen by the DMA engine (same as FMC +0x80). */
u32  nand_fifo_read(void);
u32  nand_fifo_read_dma(void);
void nand_fifo_write(u32 val);

/* Synthesize/read one raw NAND page (data + spare) for bank/page. */
void nand_get_page(u32 bank, u32 pn, u8 *data, u8 *spare);

/* Transport diagnostics: status-row text (<= 50 chars) and DMA notes. */
void nand_status_line(char *buf);
void nand_note_dma(u32 ch, u32 src, u32 dst, u32 bytes);

/* Statistics for the status rows. */
u32  nand_pages_read(void);
u32  nand_cache_misses(void);

#endif /* NAND_H */
