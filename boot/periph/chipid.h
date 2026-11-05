#ifndef CHIPID_H
#define CHIPID_H

#include "../types.h"

/*
 * S5L8900 chip ID register emulation.
 * Base address: 0x3E500000
 *
 * +0x0 = SoC number, +0x4 = chip revision << 24.
 */

void chipid_init(void);
u32  chipid_read(u32 offset);
void chipid_write(u32 offset, u32 value);

#endif /* CHIPID_H */
