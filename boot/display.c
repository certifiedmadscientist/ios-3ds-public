/*
 * display.c — iPhone display path → 3DS bottom screen
 *
 * We play the S5L8900 display controller.  The iPhone screen is a
 * 320x480 32bpp BGRA framebuffer: the boot_args Video buffer (PA
 * 0x20800000) until a display kext programs the CLCD's window-1 base
 * (+0x60, qemu-ios hw/arm/ipod_touch_lcd.c), after which we follow that.
 * On timer ticks we copy it onto the bottom screen, whose framebuffer
 * the GPU scans out as BGR8 column-major 240-pixel columns (one per
 * screen X, left to right), bottom row first:
 *   - PORTRAIT (LCD=iphone, ORIENT=portrait, the default for iphone): the
 *     panel rotated into a 240x320 portrait view with an area filter
 *     (portrait.c; hold the 3DS like a book, top screen on the left);
 *   - otherwise landscape: 1:1 at 240 lines, or 2:1 vertical halving at
 *     480 (LCD=iphone ORIENT=landscape).
 *
 * Mach trap -10 (our framebuffer trap) is redirected here by the loader
 * and draws into the iPhone framebuffer in a 320x240 coordinate space
 * (each row doubled), so test clients exercise the same path.
 */

#include "display.h"
#include "bootargs.h"
#include "console.h"
#include "iowin.h"
#include "periph/vic.h"
#include "pm.h"
#if PORTRAIT
#include "portrait.h"
#endif

#define IPHONE_W        LCD_W
#define IPHONE_H        LCD_H
#define IPHONE_STRIDE   LCD_STRIDE
#define ROWSTEP         (LCD_H / 240)     /* source rows per screen row */

#define BOT_W           320
#define BOT_H           240

#define GPU_BOT_FB_A    IOREG32(0x10400568)
#define LCD_FILL_BOT    IOREG32(0x10202A04)

/* ── CLCD register file (PA 0x38900000) ──────────────────────────────
 * AppleH1CLCD (start_hardware 0xC0381CA8, frame setup 0xC03807C4) reads
 * back the window iBoot left enabled instead of programming geometry:
 *   +0x08 bit 6/5/4/3  window 1/2/3/4 enabled
 *   +0x58 +0x5C +0x60 +0x64  window 1: stride (bytes), format (bits 10:8:
 *        6/7 = 32bpp BGRA, else RGB565), framebuffer base (inside /vram),
 *        resolution (width bits 26:16, height bits 9:0)
 * so we start with that window describing the boot framebuffer.
 * Interrupts (filter 0xC0380AE8, handler 0xC0380B40): +0x18 status,
 * write-1-to-clear; +0x14 enable mask (the driver keeps 0x3F00 underruns,
 * plus bit 0 frame-done while it has swaps pending).  A status bit outside
 * the mask is "unexpected CLCD interrupt" and gets 0xFF written back. */

#define CLCD_CON2       0x08
#define CLCD_IRQ_EN     0x14
#define CLCD_IRQ_STAT   0x18
#define CLCD_W1_SPAN    0x58
#define CLCD_W1_FMT     0x5C
#define CLCD_W1_BASE    0x60
#define CLCD_W1_RES     0x64
#define CLCD_FRAME_DONE 0x1u

static u32 clcd_regs[0x400 / 4] = {
    [CLCD_CON2 / 4]    = 0x40,
    [CLCD_W1_SPAN / 4] = LCD_STRIDE,
    [CLCD_W1_FMT / 4]  = 6u << 8,
    [CLCD_W1_BASE / 4] = BOOTARGS_VIDEO_BASE,
    [CLCD_W1_RES / 4]  = ((u32)LCD_W << 16) | LCD_H,
};

static u32 clcd_n_rd, clcd_n_wr, clcd_last_off, clcd_last_val;
static bool clcd_con2_written;      /* the kernel programmed CON2 itself */

static void clcd_update_irq(void) {
    vic_set_irq(VIC_LINE_CLCD,
                (clcd_regs[CLCD_IRQ_STAT / 4] & clcd_regs[CLCD_IRQ_EN / 4]) != 0);
}

u32 clcd_read(u32 off) {
    clcd_n_rd++;
    clcd_last_off = off;
    return off < sizeof(clcd_regs) ? clcd_regs[off >> 2] : 0;
}

void clcd_write(u32 off, u32 value) {
    clcd_n_wr++;
    clcd_last_off = off;
    clcd_last_val = value;
    if (off >= sizeof(clcd_regs))
        return;
    if (off == CLCD_IRQ_STAT) {
        clcd_regs[off >> 2] &= ~value;
        clcd_update_irq();
        return;
    }
    clcd_regs[off >> 2] = value;
    if (off == CLCD_CON2)
        clcd_con2_written = true;
    if (off == CLCD_IRQ_EN)
        clcd_update_irq();
    if (off == CLCD_W1_BASE) {
        console_print("[LCD] window1 fb=");
        console_print_hex(value);
        console_print("\n");
    }
}

/* ── Address helpers ─────────────────────────────────────────────────── */

/* VA through which our runtime can read a whole 320x480 framebuffer at
 * pa: anywhere in /vram (boot framebuffer, or the one AppleH1CLCD carves
 * out and programs into window 1) through the device window, or kernel
 * RAM through the kernel's linear map (iowin.h). */
static volatile u32 *fb_va(u32 pa) {
    if (pa - DT_VRAM_PA <= DT_VRAM_SIZE - IPHONE_STRIDE * IPHONE_H ||
        pa - 0x21000000u < 0x07000000u)
        return (volatile u32 *)io_va(pa);
    return 0;
}

static bool vram_ok(u32 pa) {
    return pa - 0x18000000u < 0x00600000u - BOT_W * BOT_H * 3;
}

/* ── Mirror ──────────────────────────────────────────────────────────── *
 * Called on every 100 Hz heartbeat with IRQs masked, so the copy is split
 * into slices: MIRROR_COLS columns per beat, a full frame every 10 beats.
 * Columns go in 8-wide tiles so each 32-byte source line (8 pixels) is
 * fetched once; output is packed 4 pixels per 3 words. */

#define MIRROR_COLS     32

#if !PORTRAIT
static void mirror_tile(volatile u32 *dst, const volatile u32 *src, u32 x0) {
    for (u32 i = 0; i < BOT_H; i += 4) {
        /* Destination index i is screen row 239-i: source row
         * (239-i)*ROWSTEP (1:1 at 240 lines, every other row at 480). */
        const volatile u32 *r0 = src + (IPHONE_H - ROWSTEP - ROWSTEP * i) * IPHONE_W + x0;
        const volatile u32 *r1 = r0 - ROWSTEP * IPHONE_W;
        const volatile u32 *r2 = r0 - 2 * ROWSTEP * IPHONE_W;
        const volatile u32 *r3 = r0 - 3 * ROWSTEP * IPHONE_W;
        volatile u32 *col = dst + x0 * (BOT_H * 3 / 4) + (i * 3 / 4);
        for (u32 c = 0; c < 8; c++) {
            u32 p0 = r0[c] & 0xFFFFFF, p1 = r1[c] & 0xFFFFFF;
            u32 p2 = r2[c] & 0xFFFFFF, p3 = r3[c] & 0xFFFFFF;
            col[0] = p0 | (p1 << 24);
            col[1] = (p1 >> 8) | (p2 << 16);
            col[2] = (p2 >> 16) | (p3 << 8);
            col += BOT_H * 3 / 4;
        }
    }
}
#endif

/* Bottom screen black (display off / asleep).  The LCD's own fill
 * (bit 24 + colour 0) overrides whatever the framebuffer holds - clearing
 * the framebuffer alone left the panel light grey/white on hardware. */
static bool fill_on;

void display_sleep_blank(void) {
    LCD_FILL_BOT = 1u << 24;
    fill_on = true;
    u32 dst_pa = GPU_BOT_FB_A;
    if (!vram_ok(dst_pa))
        return;
    volatile u32 *dst = (volatile u32 *)io_va(dst_pa);
    for (u32 i = 0; i < BOT_W * BOT_H * 3 / 4; i++)
        dst[i] = 0;
}

static u32 mirror_cyc_last, mirror_cyc_max;   /* CCNT per heartbeat slice */

static inline u32 ccnt(void) {
    u32 v;
    __asm__ volatile("mrc p15, 0, %0, c15, c12, 1" : "=r"(v));
    return v;
}

void display_mirror_step(void) {
    static bool fill_cleared;
    static u32 next_x;
    static volatile u32 *src, *dst;
    static u32 beat;

    if (++beat % 10 == 0) {             /* CLCD frame done, 10 Hz */
        clcd_regs[CLCD_IRQ_STAT / 4] |= CLCD_FRAME_DONE;
        clcd_update_irq();
    }

    if (!fill_cleared) {
        LCD_FILL_BOT = 0;           /* the fill overrides the framebuffer */
        fill_cleared = true;
    }

    if (next_x == 0) {
        /* New frame: pick up the current source and destination, and
         * drop any stale cached copy of the source (16 KB D-cache, so
         * a full clean+invalidate is cheap).  Window 1 switched off by
         * the kernel (display powered down on lock) shows black rather
         * than the last frame. */
        static bool blanked;
        bool w1_on = clcd_regs[CLCD_CON2 / 4] & 0x40;
        if (!w1_on && clcd_con2_written) {
            if (!blanked)
                display_sleep_blank();
            blanked = true;
            return;
        }
        blanked = false;
        if (fill_on) {                  /* display back on */
            LCD_FILL_BOT = 0;
            fill_on = false;
        }
        u32 src_pa = w1_on ? clcd_regs[CLCD_W1_BASE >> 2] : 0;
        if (!src_pa)
            src_pa = BOOTARGS_VIDEO_BASE;
        src = fb_va(src_pa);
        u32 dst_pa = GPU_BOT_FB_A;
        dst = vram_ok(dst_pa) ? (volatile u32 *)io_va(dst_pa) : 0;
        __asm__ volatile("mcr p15, 0, %0, c7, c14, 0\n"
                         "mcr p15, 0, %0, c7, c10, 4" :: "r"(0) : "memory");
    }
    if (!src || !dst) {
        next_x = 0;
        return;
    }
    u32 c0 = ccnt();
#if PORTRAIT
    static bool pt_ready;
    static u32 rowbuf[PT_SRC_W];
    if (!pt_ready) {
        pt_init();
        pt_ready = true;
    }
    for (u32 x = next_x; x < next_x + MIRROR_COLS; x++)
        pt_render_column(dst + x * (BOT_H * 3 / 4), src, x, rowbuf);
#else
    for (u32 x = next_x; x < next_x + MIRROR_COLS; x += 8)
        mirror_tile(dst, src, x);
#endif
    mirror_cyc_last = ccnt() - c0;
    if (mirror_cyc_last > mirror_cyc_max)
        mirror_cyc_max = mirror_cyc_last;
    next_x += MIRROR_COLS;
    if (next_x >= BOT_W)
        next_x = 0;
}

/* ── Framebuffer trap (Mach trap -10) ────────────────────────────────── *
 * args[0] op: 0 fill_rect(bgr, x, y, w, h)   1 pixel(x, y, bgr)
 *             2 get_info -> screen width
 *             3 blit(src, stride, x | y << 16, w, h): copy a user rectangle
 *               of 32bpp pixels (bytes A, R, G, B) with the kernel's copyin
 * Coordinates are 320x240; with LCD=iphone each row lands on two rows. */

static void fb_put(u32 x, u32 y, u32 bgr) {
    volatile u32 *fb = (volatile u32 *)io_va(BOOTARGS_VIDEO_BASE);
    u32 px = 0xFF000000u | bgr;
    for (u32 r = 0; r < ROWSTEP; r++)
        fb[(ROWSTEP * y + r) * IPHONE_W + x] = px;
}

u32 kern_fb_traps;              /* trap -10 calls, for the status row */

u32 htl_fb_trap(const u32 *args) {
    extern void htl_status_redraw(void);
    if ((++kern_fb_traps & 0x3FF) == 0)
        htl_status_redraw();
    switch (args[0]) {
    case 2:
        return BOT_W;
    case 1:
        if (args[1] < BOT_W && args[2] < BOT_H)
            fb_put(args[1], args[2], args[3] & 0xFFFFFF);
        return 0;
    case 0: {
        u32 x0 = args[2], y0 = args[3];
        u32 x1 = x0 + args[4], y1 = y0 + args[5];
        if (x1 > BOT_W) x1 = BOT_W;
        if (y1 > BOT_H) y1 = BOT_H;
        for (u32 y = y0; y < y1; y++)
            for (u32 x = x0; x < x1; x++)
                fb_put(x, y, args[1] & 0xFFFFFF);
        return 0;
    }
    case 3: {
        /* copyin(user, kernel, len) -> 0 or errno (kernel VA 0xC0061348) */
        typedef int (*copyin_fn)(u32 uaddr, void *kaddr, u32 len);
        static u8 row[BOT_W * 4];
        u32 src = args[1], stride = args[2];
        u32 x0 = args[3] & 0xFFFF, y0 = args[3] >> 16;
        u32 w = args[4], h = args[5];
        if (x0 >= BOT_W || y0 >= BOT_H)
            return 0;
        if (w > BOT_W - x0) w = BOT_W - x0;
        if (h > BOT_H - y0) h = BOT_H - y0;
        for (u32 y = 0; y < h; y++) {
            if (((copyin_fn)0xC0061348)(src + y * stride, row, w * 4))
                return 14;  /* EFAULT */
            for (u32 x = 0; x < w; x++) {
                const u8 *sp = &row[4 * x];
                fb_put(x0 + x, y0 + y, sp[3] | (sp[2] << 8) | (sp[1] << 16));
            }
        }
        return 0;
    }
    default:
        return 0xFFFFFFFFu;
    }
}

/* ── Boot test pattern ───────────────────────────────────────────────── */

/* Colour bars across the top of the iPhone screen, dark grey below, so
 * the first mirrored frame shows orientation and channel order. */
void display_boot_pattern(void) {
    static const u32 bars[8] = {    /* BGRA words: B low */
        0xFFFFFFFF, 0xFF00FFFF, 0xFFFFFF00, 0xFF00FF00,
        0xFFFF00FF, 0xFF0000FF, 0xFFFF0000, 0xFF000000,
    };
    volatile u32 *fb = (volatile u32 *)io_va(BOOTARGS_VIDEO_BASE);
    for (u32 y = 0; y < IPHONE_H; y++)
        for (u32 x = 0; x < IPHONE_W; x++)
            fb[y * IPHONE_W + x] = y < 80 ? bars[x / 40] : 0xFF202020;
}

/* Status row: CLCD register traffic (zero means AppleH1CLCD never got as
 * far as mapping and programming its registers) and last offset, sleep
 * state "P<A awake|R sleep requested|S asleep><requests><sleeps>" plus
 * w/- for CLCD window 1 on/off, window 1 base, SPI traffic,
 * mirror kilocycles per heartbeat slice (last/max; 32 of 320 columns). */
extern u32 spi_tx_total(int n);

static char *row_hex(char *p, char tag, u32 v, u32 digits) {
    static const char h[] = "0123456789ABCDEF";
    *p++ = tag;
    for (int i = (int)digits - 1; i >= 0; i--)
        *p++ = h[(v >> (i * 4)) & 0xF];
    return p;
}

void display_status_line(char *buf) {
    char *p = buf;
    p = row_hex(p, 'C', clcd_n_rd, 4);
    p = row_hex(p, '/', clcd_n_wr, 4);
    p = row_hex(p, ' ', clcd_last_off, 3);
    p = pm_status(p);
    *p++ = (clcd_regs[CLCD_CON2 / 4] & 0x40) ? 'w' : '-';   /* window 1 */
    p = row_hex(p, ' ', clcd_regs[CLCD_W1_BASE >> 2], 8);
    p = row_hex(p, ' ', spi_tx_total(0), 3);
    p = row_hex(p, '/', spi_tx_total(1), 3);
    /* mirror cost per heartbeat slice, kilocycles: last/max */
    p = row_hex(p, ' ', mirror_cyc_last >> 10, 3);
    p = row_hex(p, '/', mirror_cyc_max >> 10, 3);
    *p = 0;
}
