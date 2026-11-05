/*
 * S5L8900 chip ID registers, PA 0x3E500000 (device tree "chipid").
 *
 * +0x0 returns the SoC number; +0x4 holds the chip revision in bits
 * 31:24, which qemu-ios reports as 2.
 */

#include "chipid.h"

void chipid_init(void)
{
}

u32 chipid_read(u32 offset)
{
    switch (offset) {
    case 0x0:
        return 0x00008900;
    case 0x4:
        return 2u << 24;
    default:
        return 0;
    }
}

void chipid_write(u32 offset, u32 value)
{
    (void)offset;
    (void)value;
}
