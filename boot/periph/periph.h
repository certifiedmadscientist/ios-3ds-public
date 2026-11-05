#ifndef PERIPH_H
#define PERIPH_H

#include "../types.h"

/*
 * Called by the HTL when an IO access is trapped.
 * addr:     S5L8900 peripheral address being accessed
 * value:    for writes, the value being written; for reads, ignored
 * is_write: true = store, false = load
 * width:    access width in bytes (1, 2, or 4)
 * Returns:  for reads, the value to return to the kernel; for writes, ignored
 */
u32 periph_dispatch(u32 addr, u32 value, bool is_write, u32 width);

/* Initialize all peripheral emulators */
void periph_init(void);

#endif /* PERIPH_H */
