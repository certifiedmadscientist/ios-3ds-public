#ifndef SYSIC_H
#define SYSIC_H

#include "../types.h"

/*
 * S5L8900 system IC / power controller at 0x39A00000: power gating
 * state and GPIO interrupt registers (qemu-ios model).
 */

void sysic_init(void);
u32  sysic_read(u32 offset);
void sysic_write(u32 offset, u32 value);

/* Latch GPIO interrupt irq (group irq/32, bit irq%32) and update the
 * group's VIC line. */
void sysic_gpio_raise(u32 irq);

/* Whether irq is latched in its group's INTSTAT. */
bool sysic_gpio_pending(u32 irq);

/* Diagnostics for irq: bit 0 latched, bit 1 enabled, bits 8+ the number
 * of INTSTAT writes that cleared it (kernel acks). */
u32  sysic_gpio_state(u32 irq);
void sysic_gpio_diag(u32 irq, u32 *raises, u32 *acks, u32 *bits);

#endif /* SYSIC_H */
