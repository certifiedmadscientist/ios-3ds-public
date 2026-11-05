#ifndef CLOCK_H
#define CLOCK_H

#include "../types.h"

/*
 * S5L8900 clock controllers: clock0 at 0x38100000, clock1 at 0x3C500000.
 * Fixed PLL configuration with all PLLs locked (qemu-ios model).
 */

void clock_init(void);
u32  clock_read(int controller, u32 offset);
void clock_write(int controller, u32 offset, u32 value);

#endif /* CLOCK_H */
