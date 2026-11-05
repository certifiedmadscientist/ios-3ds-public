#ifndef INPUT_H
#define INPUT_H

#include "../types.h"

/* Touchscreen + pad (3DS) feeding the iPod's multitouch and buttons. */
void input_init(void);
void input_poll(u32 beat);
void input_status_line(char *buf);
void input_button_line(char *buf);  /* Hold/Home GPIO + PMU wake row */

u8   mt_transfer(u8 value);         /* SPI2 multitouch byte */
void mt_cs(void);                   /* SPI2 transaction (chip select) start */
void mt_tick(u32 now_ms);           /* re-announce queued frames */
void input_touch_sample(bool down, u32 raw_x, u32 raw_y, u32 now_ms);
u32  input_gpio_read(u32 offset);   /* GPIO port data, e.g. +0x2C4 */
u32  input_pad_now(void);           /* 3DS pad, pressed bits */
u32  input_wake_buttons(u32 pad);   /* Hold/Home buttons in pad */
void input_wake(u32 pad);           /* latch the button that woke it */

#endif /* INPUT_H */
