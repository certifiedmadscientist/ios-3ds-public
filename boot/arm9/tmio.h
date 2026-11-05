/*
 * tmio.h -- TMIO SD card controller driver for the 3DS ARM9
 *
 * The TMIO (SD/MMC) controller is at 0x10006000 and is accessible
 * only from the ARM9 processor.
 */

#ifndef TMIO_H
#define TMIO_H

#include "types9.h"

/*
 * Initialize the SD card via the TMIO controller.
 *
 * skip_card_init: when true, assumes the card was already initialized
 *   by the boot chain (boot9strap / Luma3DS) and skips CMD0-CMD3.
 *   The controller registers are still configured for data reads.
 *
 * Returns true on success.
 */
bool tmio_init(bool skip_card_init);

/*
 * Read 'count' 512-byte sectors starting at logical sector 'sector'
 * into the buffer at 'buf'.  SDHC sector addressing is used.
 *
 * Returns true on success.
 */
bool tmio_read_sectors(u32 sector, u32 count, void *buf);

/* Return last ISTAT32 value seen (for diagnostics after a failure). */
u32 tmio_last_istat(void);

#endif /* TMIO_H */
