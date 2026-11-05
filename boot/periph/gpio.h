#ifndef GPIO_H
#define GPIO_H

#include "../types.h"

/*
 * S5L8900 GPIO at 0x3E400000.  Writes are ignored; reads return 0
 * (qemu-ios model).
 */

void gpio_init(void);
u32  gpio_read(u32 offset);
void gpio_write(u32 offset, u32 value);

#endif /* GPIO_H */
