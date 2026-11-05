#ifndef HW_3DS_H
#define HW_3DS_H

#include "types.h"

/* ── Memory regions ──────────────────────────────────────────────────── */

#define FCRAM_BASE          0x20000000
#define FCRAM_SIZE_O3DS     (128 * 1024 * 1024)   /* 128 MB Old 3DS */
#define FCRAM_SIZE_N3DS     (256 * 1024 * 1024)   /* 256 MB New 3DS */

#define VRAM_BASE           0x18000000
#define VRAM_SIZE           (6 * 1024 * 1024)     /* 6 MB */

/* ── Stack ───────────────────────────────────────────────────────────── */

#define SVC_STACK_TOP       0x28000000  /* Top of FCRAM (Old 3DS) */

/* ── GPU / LCD registers ─────────────────────────────────────────────── */

#define GPU_REG_BASE        0x10400000

/* Top screen */
#define REG_TOP_FMT         (*(vu32 *)0x10400400)
#define REG_TOP_STRIDE      (*(vu32 *)0x1040045C)
#define REG_TOP_FB_LEFT_A   (*(vu32 *)0x10400468)
#define REG_TOP_FB_LEFT_B   (*(vu32 *)0x1040046C)
#define REG_TOP_FB_RIGHT_A  (*(vu32 *)0x10400470)
#define REG_TOP_FB_RIGHT_B  (*(vu32 *)0x10400474)
#define REG_TOP_FB_SELECT   (*(vu32 *)0x10400478)

/* Bottom screen */
#define REG_BOT_FMT         (*(vu32 *)0x10400500)
#define REG_BOT_STRIDE      (*(vu32 *)0x1040055C)
#define REG_BOT_FB_A        (*(vu32 *)0x10400568)
#define REG_BOT_FB_B        (*(vu32 *)0x1040056C)
#define REG_BOT_FB_SELECT   (*(vu32 *)0x10400578)

/* ── Framebuffer layout ──────────────────────────────────────────────── */

#define SCREEN_TOP_WIDTH    400
#define SCREEN_TOP_HEIGHT   240
#define SCREEN_BOT_WIDTH    320
#define SCREEN_BOT_HEIGHT   240

/* RGB8: 3 bytes per pixel */
#define FB_BPP              3

/* Screens are column-major (rotated 90 CCW).
   Pixel (x,y) on a WxH screen => byte offset = ((H-1-y) + x*H) * BPP */
#define FB_PIXEL_OFFSET(x, y, h)  (((h) - 1 - (y)) + (x) * (h))

/* Framebuffer format register bits [2:0]: 0=RGBA8, 1=RGB8, 2=RGB565, 3=RGB5A1, 4=RGBA4 */
#define FB_FMT_RGB8         0x00000001

/* Stride for RGB8, 240 rows: 240 * 3 = 720 */
#define FB_STRIDE_RGB8      (240 * FB_BPP)

/* Framebuffer addresses in FCRAM (after code region) */
#define FB_TOP_ADDR         0x20100000
#define FB_TOP_SIZE         (SCREEN_TOP_WIDTH * SCREEN_TOP_HEIGHT * FB_BPP)

#define FB_BOT_ADDR         0x20200000
#define FB_BOT_SIZE         (SCREEN_BOT_WIDTH * SCREEN_BOT_HEIGHT * FB_BPP)

/* ── CFG registers (for hardware info) ───────────────────────────────── */

#define CFG_UNITINFO        (*(vu8  *)0x10140FFC)

/* PDN (power domain) — used to detect New 3DS */
#define PDN_MPCORE_CLKCNT   (*(vu16 *)0x10141300)

/* ── Helper macros ───────────────────────────────────────────────────── */

static inline void reg_write32(u32 addr, u32 val) {
    *(vu32 *)addr = val;
}

static inline u32 reg_read32(u32 addr) {
    return *(vu32 *)addr;
}

#endif /* HW_3DS_H */
