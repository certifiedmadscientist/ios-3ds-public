#ifndef UART_H
#define UART_H

#include "../types.h"

/*
 * S5L8900 UART emulation.
 * UART0 at 0x3CC00000, spaced 0x4000 apart (up to UART4).
 *
 * The kernel uses UART0 for serial console output.
 * We redirect TX to the 3DS top-screen debug console.
 */

void uart_init(void);
u32  uart_read(int uart_index, u32 offset);
void uart_write(int uart_index, u32 offset, u32 value);

/* Kernel console tap (see uart.c). */
void uart_tap_idle(void);
void uart_tap_rows(char *row_a, char *row_b);
void uart_tap_nodes(char *row_a, char *row_b);

#endif /* UART_H */
