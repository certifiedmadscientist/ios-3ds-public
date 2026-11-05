/*
 * S5L8900 system IC / power controller, PA 0x39A00000 (device tree
 * "power", and the second "gpio" reg range for GPIO interrupts).
 * Follows qemu-ios ipod_touch_sysic.c.
 */

#include "sysic.h"
#include "vic.h"

#define POWER_SETSTATE  0x08
#define POWER_ONCTRL    0x0C
#define POWER_OFFCTRL   0x10
#define POWER_STATE     0x14
#define POWER_ID        0x44
#define POWER_ID_ADM    0x10

#define GPIO_INTLEVEL   0x80
#define GPIO_INTSTAT    0xA0
#define GPIO_INTEN      0xC0
#define GPIO_INTTYPE    0xE0
#define GPIO_GROUPS     7

static u32 power_state;
static u32 gpio_int_level[GPIO_GROUPS];
static u32 gpio_int_status[GPIO_GROUPS];
static u32 gpio_int_enabled[GPIO_GROUPS];
static u32 gpio_int_type[GPIO_GROUPS];
static u16 gpio_acks[GPIO_GROUPS * 32];   /* INTSTAT clears per irq */
static u16 gpio_raises[GPIO_GROUPS * 32]; /* sysic_gpio_raise per irq */
static u32 gpio_level_written[GPIO_GROUPS]; /* INTLEVEL writes (reads stay 0) */

/* VIC line of each GPIO interrupt group (device tree gpio "interrupts"). */
static const u8 group_vic_line[GPIO_GROUPS] = { 0x21, 0x20, 0x1F, 0x03, 0x02, 0x01, 0x00 };

static void gpio_update(int g)
{
    vic_set_irq(group_vic_line[g], (gpio_int_status[g] & gpio_int_enabled[g]) != 0);
}

void sysic_gpio_raise(u32 irq)
{
    int g = (int)(irq / 32);
    if (g >= GPIO_GROUPS)
        return;
    gpio_int_status[g] |= 1u << (irq % 32);
    gpio_raises[irq]++;
    gpio_update(g);
}

bool sysic_gpio_pending(u32 irq)
{
    u32 g = irq / 32;
    return g < GPIO_GROUPS && (gpio_int_status[g] & (1u << (irq % 32)));
}

/* Diagnostics: raises, acks, and bit 0 enabled / 1 type / 2 level
 * (as last written by the kernel) for irq. */
void sysic_gpio_diag(u32 irq, u32 *raises, u32 *acks, u32 *bits)
{
    u32 g = irq / 32, bit = 1u << (irq % 32);
    if (g >= GPIO_GROUPS) {
        *raises = *acks = *bits = 0;
        return;
    }
    *raises = gpio_raises[irq];
    *acks = gpio_acks[irq];
    *bits = ((gpio_int_enabled[g] & bit) ? 1u : 0) | ((gpio_int_type[g] & bit) ? 2u : 0) |
            ((gpio_level_written[g] & bit) ? 4u : 0);
}

u32 sysic_gpio_state(u32 irq)
{
    u32 g = irq / 32, bit = 1u << (irq % 32);
    if (g >= GPIO_GROUPS)
        return 0;
    return ((gpio_int_status[g] & bit) ? 1u : 0) | ((gpio_int_enabled[g] & bit) ? 2u : 0) |
           ((u32)gpio_acks[irq] << 8);
}

/* Group index for a GPIO interrupt register bank at base, or -1. */
static int gpio_group(u32 offset, u32 base)
{
    if (offset < base || offset >= base + GPIO_GROUPS * 4)
        return -1;
    return (int)((offset - base) / 4);
}

void sysic_init(void)
{
    power_state = 0;
}

u32 sysic_read(u32 offset)
{
    int g;

    switch (offset) {
    case POWER_ID:
        return 2u << 24;
    case POWER_SETSTATE:
    case POWER_STATE:
        return power_state;
    case 0x7A:
    case 0x7C:
        return 1;
    }
    if ((g = gpio_group(offset, GPIO_INTLEVEL)) >= 0) return gpio_int_level[g];
    if ((g = gpio_group(offset, GPIO_INTSTAT)) >= 0)  return gpio_int_status[g];
    if ((g = gpio_group(offset, GPIO_INTEN)) >= 0)    return gpio_int_enabled[g];
    if ((g = gpio_group(offset, GPIO_INTTYPE)) >= 0)  return gpio_int_type[g];
    return 0;
}

void sysic_write(u32 offset, u32 value)
{
    int g;

    switch (offset) {
    case POWER_ONCTRL:
        /* Don't record "on" for these, so the devices look powered up
         * immediately (qemu-ios does the same). */
        if (value & (0x20 | 0x4 | POWER_ID_ADM))
            return;
        power_state = value;
        return;
    case POWER_OFFCTRL:
        power_state = value;
        return;
    }
    if ((g = gpio_group(offset, GPIO_INTSTAT)) >= 0) {
        for (u32 cleared = gpio_int_status[g] & value, b = 0; cleared; b++, cleared >>= 1)
            if (cleared & 1)
                gpio_acks[g * 32 + b]++;
        gpio_int_status[g] &= ~value;
        gpio_update(g);
    } else if ((g = gpio_group(offset, GPIO_INTEN)) >= 0) {
        gpio_int_enabled[g] = value;
        gpio_update(g);
    }
    else if ((g = gpio_group(offset, GPIO_INTTYPE)) >= 0)
        gpio_int_type[g] = value;
    else if ((g = gpio_group(offset, GPIO_INTLEVEL)) >= 0)
        gpio_level_written[g] = value;     /* recorded only, as qemu-ios */
}
