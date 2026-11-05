#include "screen.h"
#include "3ds_hw.h"
#include "iowin.h"

/* ── Helpers ─────────────────────────────────────────────────────────── */

static void memset32(void *dst, u8 val, u32 size) {
    u8 *p = (u8 *)dst;
    for (u32 i = 0; i < size; i++)
        p[i] = val;
}

u8 *screen_top_fb(void) {
    return (u8 *)io_va(screen_top_fb_addr);
}

u8 *screen_bot_fb(void) {
    return (u8 *)io_va(screen_bot_fb_addr);
}

/* ── Screen initialization ───────────────────────────────────────────── */

void screen_init(void) {
    /* Don't reconfigure GPU — use Luma's existing framebuffer setup.
     * Just clear to black using whatever FB addresses are already set. */
    volatile u32 *bot_reg = (volatile u32 *)io_va(0x10400568);
    volatile u32 *top_reg = (volatile u32 *)io_va(0x10400468);
    u32 bot_fb = *bot_reg;
    u32 top_fb = *top_reg;

    /* Store the addresses for our screen functions to use */
    *(volatile u32 *)&screen_top_fb_addr = top_fb;
    *(volatile u32 *)&screen_bot_fb_addr = bot_fb;

    /* Clear both screens to black */
    memset32((void *)io_va(top_fb), 0, FB_TOP_SIZE);
    memset32((void *)io_va(bot_fb), 0, FB_BOT_SIZE);
}

/* Runtime FB addresses — set by screen_init from GPU registers */
u32 screen_top_fb_addr;
u32 screen_bot_fb_addr;

/* ── Pixel plotting ──────────────────────────────────────────────────── */

/*
 * 3DS framebuffers are column-major, rotated 90 degrees CCW.
 * For a screen of width W and height H (in the natural landscape orientation):
 *   pixel (x, y) is at byte offset = ((H-1-y) + x*H) * 3
 * Byte order in memory is B, G, R (for RGB8 format).
 */

void screen_top_pixel(u32 x, u32 y, u8 r, u8 g, u8 b) {
    if (x >= SCREEN_TOP_WIDTH || y >= SCREEN_TOP_HEIGHT)
        return;
    u32 off = FB_PIXEL_OFFSET(x, y, SCREEN_TOP_HEIGHT) * FB_BPP;
    u8 *fb = screen_top_fb();
    fb[off + 0] = b;
    fb[off + 1] = g;
    fb[off + 2] = r;
}

void screen_bot_pixel(u32 x, u32 y, u8 r, u8 g, u8 b) {
    if (x >= SCREEN_BOT_WIDTH || y >= SCREEN_BOT_HEIGHT)
        return;
    u32 off = FB_PIXEL_OFFSET(x, y, SCREEN_BOT_HEIGHT) * FB_BPP;
    u8 *fb = screen_bot_fb();
    fb[off + 0] = b;
    fb[off + 1] = g;
    fb[off + 2] = r;
}

/* ── Fills ───────────────────────────────────────────────────────────── */

void screen_top_fill(u8 r, u8 g, u8 b) {
    u8 *fb = screen_top_fb();
    for (u32 i = 0; i < SCREEN_TOP_WIDTH * SCREEN_TOP_HEIGHT; i++) {
        fb[i * 3 + 0] = b;
        fb[i * 3 + 1] = g;
        fb[i * 3 + 2] = r;
    }
}

void screen_bot_fill(u8 r, u8 g, u8 b) {
    u8 *fb = screen_bot_fb();
    for (u32 i = 0; i < SCREEN_BOT_WIDTH * SCREEN_BOT_HEIGHT; i++) {
        fb[i * 3 + 0] = b;
        fb[i * 3 + 1] = g;
        fb[i * 3 + 2] = r;
    }
}

/* ── Gradient ────────────────────────────────────────────────────────── */

void screen_bot_gradient(void) {
    for (u32 x = 0; x < SCREEN_BOT_WIDTH; x++) {
        /* Blue intensity increases left-to-right */
        u8 blue = (u8)((x * 255) / (SCREEN_BOT_WIDTH - 1));
        /* Slight green tint at the top */
        for (u32 y = 0; y < SCREEN_BOT_HEIGHT; y++) {
            u8 green = (u8)((y * 60) / (SCREEN_BOT_HEIGHT - 1));
            screen_bot_pixel(x, y, 0, green, blue);
        }
    }
}
