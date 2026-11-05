#ifndef SCREEN_H
#define SCREEN_H

#include "types.h"

/* Initialize both screens (set GPU registers, clear framebuffers). */
void screen_init(void);

/* Plot a single pixel on the top screen (400x240, RGB8). */
void screen_top_pixel(u32 x, u32 y, u8 r, u8 g, u8 b);

/* Plot a single pixel on the bottom screen (320x240, RGB8). */
void screen_bot_pixel(u32 x, u32 y, u8 r, u8 g, u8 b);

/* Fill the top screen with a solid color. */
void screen_top_fill(u8 r, u8 g, u8 b);

/* Fill the bottom screen with a solid color. */
void screen_bot_fill(u8 r, u8 g, u8 b);

/* Draw a blue gradient on the bottom screen. */
void screen_bot_gradient(void);

/* Get pointer to top framebuffer. */
u8 *screen_top_fb(void);

/* Get pointer to bottom framebuffer. */
u8 *screen_bot_fb(void);

/* Runtime FB addresses (set by screen_init from GPU registers) */
extern u32 screen_top_fb_addr;
extern u32 screen_bot_fb_addr;

#endif /* SCREEN_H */
