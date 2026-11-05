#ifndef VIC_H
#define VIC_H

#include "../types.h"

/* VIC0 lines 0-31, VIC1 lines 32-63 (device-tree "interrupts" numbering). */
#define VIC_LINE_SPI0       9
#define VIC_LINE_SPI1       10
#define VIC_LINE_SPI2       11
#define VIC_LINE_CLCD       13
#define VIC_LINE_I2C0       21
#define VIC_LINE_I2C1       22

void vic_init(void);
u32  vic_read(int vic_index, u32 offset);
void vic_write(int vic_index, u32 offset, u32 value);

/* Drive a device's interrupt line (level-triggered). */
void vic_set_irq(int irq_num, bool level);

/* VIC0's CPU IRQ output is asserted (an IRQ-selected line wants service). */
bool vic_irq_pending(void);
/* Any enabled FIQ-selected line is asserted. */
bool vic_fiq_pending(void);
/* IRQs acknowledged by the kernel per line (VECTADDR reads). */
u32  vic_line_count(int line);

/* acks, EOIs, current, prio mask, stack depth, VIC0 enable, raw|soft */
void vic_debug(u32 out[7]);

/* Provided by the IRQ bridge (htl.c): request prompt delivery. */
void vic_kick(void);

#endif /* VIC_H */
