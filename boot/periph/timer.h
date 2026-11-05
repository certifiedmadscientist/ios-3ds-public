#ifndef TIMER_H
#define TIMER_H

#include "../types.h"

/*
 * S5L8900 timer emulation at PA 0x3E200000: the kernel's 64-bit timebase
 * counter and its deadline (scheduler tick) timer.  The MPCore private
 * timer (GIC ID 29) runs a fixed 100 Hz heartbeat; deadlines are checked
 * against the counter on each beat.
 */

void timer_init(void);
u32  timer_read(u32 offset);
void timer_write(u32 offset, u32 value);
u64  timer_get_ticks(void);

bool timer_heartbeat(void);

/* GIC software interrupt the IRQ bridge sends itself. */
#define KICK_SGI_ID     1

#endif /* TIMER_H */
