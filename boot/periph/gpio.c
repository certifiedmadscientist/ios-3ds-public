/*
 * S5L8900 GPIO, PA 0x3E400000 (device tree "gpio").  Like qemu-ios:
 * writes are ignored and every register reads 0 except the button state
 * at +0x2C4 (bit 5 hold, bit 6 menu; periph/input.c).
 */

#include "gpio.h"
#include "input.h"

void gpio_init(void)
{
}

u32 gpio_read(u32 offset)
{
    return input_gpio_read(offset);
}

void gpio_write(u32 offset, u32 value)
{
    (void)offset;
    (void)value;
}
