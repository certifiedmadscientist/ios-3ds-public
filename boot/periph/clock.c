/*
 * S5L8900 clock controllers: clock0 at PA 0x38100000, clock1 at
 * PA 0x3C500000 (device tree "clkrstgen").  Both use the clock1 model
 * from qemu-ios: fixed PLL/divider configuration taken from a real
 * iPod touch, all four PLLs locked, everything else reads 0.
 */

#include "clock.h"

#define CLOCK_CONFIG0   0x00
#define CLOCK_CONFIG1   0x04
#define CLOCK_CONFIG2   0x08
#define CLOCK_PLL0CON   0x20
#define CLOCK_PLL1CON   0x24
#define CLOCK_PLL2CON   0x28
#define CLOCK_PLL3CON   0x2C
#define CLOCK_PLLLOCK   0x40
#define CLOCK_PLLMODE   0x44

static u32 plllock[2];

void clock_init(void)
{
    plllock[0] = plllock[1] = 0xF;
}

u32 clock_read(int controller, u32 offset)
{
    if (controller < 0 || controller > 1)
        return 0;

    switch (offset) {
    case CLOCK_CONFIG0:     /* CPU on PLL1, memory divisor 2 */
        return (1u << 12) | (1u << 24) | (2u << 16);
    case CLOCK_CONFIG1:     /* bus on PLL1 /3, peripheral factor 1 */
        return (1u << 12) | (1u << 24) | (3u << 16) | (1u << 8) | 3u |
               (1u << 20) | (1u << 14) | (1u << 28) | (1u << 30);
    case CLOCK_CONFIG2:     /* peripheral PLL 3, display divisor 1 */
        return (3u << 28) | (1u << 24) | (1u << 16);
    case CLOCK_PLL0CON:     /* MDIV << 8 | PDIV << 24 | SDIV */
        return (80u << 8) | (8u << 24) | 0;
    case CLOCK_PLL1CON:
        return (103u << 8) | (6u << 24) | 0;
    case CLOCK_PLL2CON:
        return (156u << 8) | (53u << 24) | 2;
    case CLOCK_PLL3CON:
        return (72u << 8) | (8u << 24) | 1;
    case CLOCK_PLLLOCK:
        return plllock[controller];
    case CLOCK_PLLMODE:
        return 0x000A003A;
    default:
        return 0;
    }
}

void clock_write(int controller, u32 offset, u32 value)
{
    if (controller < 0 || controller > 1)
        return;
    if (offset == CLOCK_PLLLOCK)
        plllock[controller] = value;
}
