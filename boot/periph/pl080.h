/*
 * pl080.h — ARM PL080 DMA controllers of the S5L8900
 *
 *   dmac0  0x38200000  IRQ 16  (flash controller, ...)
 *   dmac1  0x39900000  IRQ 17
 */

#ifndef PL080_H
#define PL080_H

#include "../types.h"

#define PL080_DMAC0_BASE  0x38200000u
#define PL080_DMAC1_BASE  0x39900000u

void pl080_init(void);
u32  pl080_read(int dmac, u32 off);
void pl080_write(int dmac, u32 off, u32 val);

/* Bytes moved since boot, for the status rows. */
u32  pl080_bytes(void);

/* Run paced (device-endpoint) channels as far as their devices allow. */
void pl080_service(void);

/* Bytes the device register at pa can accept (to_device) or provide. */
u32 periph_dma_avail(u32 pa, bool to_device);

/* An enabled DMA channel still writes to device register pa. */
bool pl080_feeds(u32 pa);

/* Heartbeat: resume channels left pending by a slice cap. */
void pl080_tick(void);

/* Hang breadcrumb: a nibble stack of the emulation routines running now.
 *   1 htl_dispatch  2 DMA run_channel  3 DMA run_paced  4 spi_run
 *   5 NAND scatter list */
extern volatile u32 htl_crumb;
#define CRUMB_IN(id)  (htl_crumb = (htl_crumb << 4) | (id))
#define CRUMB_OUT()   (htl_crumb >>= 4)

/* Loop-cap report: prints "what a b c e K<crumb>" once per flag. */
void hang_cap(bool *shown, const char *what, u32 a, u32 b, u32 c, u32 e);

#endif /* PL080_H */
