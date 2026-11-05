#ifndef CONSOLE_H
#define CONSOLE_H

#include "types.h"

/* Initialize the text console on the top screen.
   Call screen_init() first. */
void console_init(void);

/* Clear the top screen, move the cursor to row 0 and wrap (not scroll)
 * from then on. */
void console_home(void);

/* Draw s on pinned status line `line` (0 or 1) at the bottom of the screen;
 * wrapped log output never touches these rows. */
void console_status(u32 line, const char *s);

/* Print a single character to the console. */
void console_putc(char c);

/* Print a null-terminated string to the console.
   Supports '\n' for newline. Scrolls when the screen is full. */
void console_print(const char *str);

/* Print a 32-bit value as 8-digit hex. */
void console_print_hex(u32 val);

#endif /* CONSOLE_H */
