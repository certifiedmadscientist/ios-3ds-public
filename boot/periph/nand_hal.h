/*
 * nand_hal.h — what the NAND/DMA emulation needs from its environment.
 *
 * Implemented by nand_hal_3ds.c on the 3DS (ARM9 block server, kernel
 * physical memory, VIC lines) and by tools/nand_harness on the host.
 */

#ifndef NAND_HAL_H
#define NAND_HAL_H

#include "../types.h"

/* Read cache: NHAL_SLOTS slots of NHAL_SLOT_PAGES 2048-byte disk pages. */
#define NHAL_SLOT_PAGES   32u
#define NHAL_SLOT_BYTES   (NHAL_SLOT_PAGES * 2048u)
#define NHAL_SLOTS        32u

/* CPU pointer to cache slot n (NHAL_SLOT_BYTES long). */
u8  *nhal_slot(u32 n);
/* Fill slot n with `count` 512-byte sectors of the disk image starting at
 * `sector`.  Returns false on any backend error. */
bool nhal_fill(u32 n, u32 sector, u32 count);
/* Disk image size in 512-byte sectors (0 if no image is available). */
u32  nhal_disk_sectors(void);

/* Backing store for programmed pages: NHAL_OVL_PAGES records of
 * NHAL_OVL_REC bytes (2048 data + 64 spare), uninitialized. */
#define NHAL_OVL_PAGES    7680u
#define NHAL_OVL_REC      (2048u + 64u)
u8  *nhal_overlay(void);

/* Access to the emulated machine's physical memory (kernel RAM). */
bool nhal_phys_read(u32 pa, void *dst, u32 len);
bool nhal_phys_write(u32 pa, const void *src, u32 len);

/* Drive an interrupt line (device-tree numbering). */
void nhal_irq(int line, bool level);

/* One-line diagnostics. */
void nhal_log(const char *msg, u32 val);
void nhal_puts(const char *line);

/* 100 Hz tick count (for idle detection). */
u32  nhal_ticks(void);

#endif /* NAND_HAL_H */
