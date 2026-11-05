/*
 * portrait.c - rotate + area-filter the 320x480 iPhone panel into the
 * portrait view on the 3DS bottom screen, and map touches back (see
 * portrait.h for the geometry).  Pure integer code with no hardware
 * access, so tools/portrait_test builds it on the host too.
 */

#include "portrait.h"

typedef struct {
    u16 i0;                 /* first panel pixel */
    u16 q;                  /* weight code for i0 (see blendq), i0+1 the rest */
} pt_tap_t;

static pt_tap_t xtap[PT_IMG_W];     /* picture column u' -> panel x */
static pt_tap_t ytap[PT_VIEW_H];    /* view row v -> panel y */

/* Weight of the first pixel, in eighths, for codes 0..6. */
static const u8 q_w8[7] = { 8, 6, 5, 4, 3, 2, 0 };

/* Output k of n_out covers panel interval [k*n_src, (k+1)*n_src) in units
 * of 1/n_out pixel; with n_src/n_out <= 1.5 that is at most two pixels.
 * The exact area weight is rounded to the nearest of 1, 3/4, 5/8, 1/2,
 * 3/8, 1/4, 0 (exact for 3/4 and 1/2; 2/3 becomes 5/8). */
static void taps(pt_tap_t *t, u32 n_out, u32 n_src) {
    for (u32 k = 0; k < n_out; k++) {
        u32 s = k * n_src, e = s + n_src;
        u32 i0 = s / n_out;
        u32 b = (i0 + 1) * n_out;           /* end of pixel i0 */
        u32 cov0 = (e < b ? e : b) - s;     /* weight = cov0 / n_src */
        u32 best = 0, err = ~0u;
        for (u32 q = 0; q < 7; q++) {
            u32 a = cov0 * 8, c = q_w8[q] * n_src;
            u32 d = a > c ? a - c : c - a;
            if (d < err) {
                err = d;
                best = q;
            }
        }
        if (i0 + 1 >= n_src)                /* last pixel: no neighbour */
            best = 0;
        t[k].i0 = (u16)i0;
        t[k].q = (u16)best;
    }
}

void pt_init(void) {
    taps(xtap, PT_IMG_W, PT_SRC_W);
    taps(ytap, PT_VIEW_H, PT_SRC_H);
}

/* Per-byte (a + b) / 2, rounding down: ARMv6 UHADD8, all four channels in
 * one instruction (the alpha byte is carried along and dropped later). */
static inline u32 h8(u32 a, u32 b) {
#if defined(__arm__)
    u32 r;
    __asm__("uhadd8 %0, %1, %2" : "=r"(r) : "r"(a), "r"(b));
    return r;
#else
    return (a & b) + (((a ^ b) & 0xFEFEFEFEu) >> 1);
#endif
}

/* a*w + b*(1-w) for the weight code q (w = q_w8[q] / 8). */
static inline u32 blendq(u32 a, u32 b, u32 q) {
    if (q > 3) {                        /* mirror: weight 1-w on b */
        u32 s = a;
        a = b;
        b = s;
        q = 6 - q;
    }
    if (q == 0)
        return a;
    u32 m = h8(a, b);                   /* 1/2 */
    if (q == 3)
        return m;
    u32 t = h8(a, m);                   /* 3/4 */
    return q == 1 ? t : h8(m, t);       /* 5/8 */
}

void pt_view_to_screen(u32 u, u32 v, u32 *X, u32 *Y) {
#if PORTRAIT_CCW
    *X = 319 - v;
    *Y = u;
#else
    *X = v;
    *Y = 239 - u;
#endif
}

void pt_render_column(volatile u32 *dstcol, const volatile u32 *src, u32 X, u32 *rowbuf) {
#if PORTRAIT_CCW
    u32 v = 319 - X;
#else
    u32 v = X;
#endif
    /* vertical pass: panel rows y, y+1 -> one row */
    u32 y = ytap[v].i0, qy = ytap[v].q;
    const volatile u32 *r0 = src + y * PT_SRC_W;
    const volatile u32 *r1 = qy == 0 ? r0 : r0 + PT_SRC_W;
    if (qy > 3) {                       /* weight mostly on the lower row */
        const volatile u32 *s = r0;
        r0 = r1;
        r1 = s;
        qy = 6 - qy;
    }
    /* one loop per weight, so the inner loop has no branches */
    u32 x;
    switch (qy) {
    case 0:
        for (x = 0; x < PT_SRC_W; x++)
            rowbuf[x] = r0[x];
        break;
    case 1:
        for (x = 0; x < PT_SRC_W; x++) {
            u32 a = r0[x];
            rowbuf[x] = h8(a, h8(a, r1[x]));
        }
        break;
    case 2:
        for (x = 0; x < PT_SRC_W; x++) {
            u32 a = r0[x], m = h8(a, r1[x]);
            rowbuf[x] = h8(m, h8(a, m));
        }
        break;
    default:
        for (x = 0; x < PT_SRC_W; x++)
            rowbuf[x] = h8(r0[x], r1[x]);
        break;
    }

    /* horizontal pass into the column, index i = 239 - Y (bottom first) */
    static u32 pix[PT_VIEW_W];      /* static: runs on the IRQ stack */
    for (u32 u = 0; u < PT_VIEW_W; u++) {
        u32 p = 0;
        if (u - PT_IMG_X < PT_IMG_W) {
            const pt_tap_t *t = &xtap[u - PT_IMG_X];
            u32 x0 = t->i0, x1 = t->q == 0 ? x0 : x0 + 1;
            p = blendq(rowbuf[x0], rowbuf[x1], t->q) & 0xFFFFFF;
        }
#if PORTRAIT_CCW
        pix[239 - u] = p;           /* Y = u */
#else
        pix[u] = p;                 /* Y = 239 - u */
#endif
    }
    for (u32 i = 0; i < PT_VIEW_W; i += 4) {
        u32 p0 = pix[i], p1 = pix[i + 1], p2 = pix[i + 2], p3 = pix[i + 3];
        *dstcol++ = p0 | (p1 << 24);
        *dstcol++ = (p1 >> 8) | (p2 << 16);
        *dstcol++ = (p2 >> 16) | (p3 << 8);
    }
}

void pt_touch(u32 X, u32 Y, u32 *fx, u32 *fy) {
    if (X > 319) X = 319;
    if (Y > 239) Y = 239;
#if PORTRAIT_CCW
    u32 u = Y, v = 319 - X;
#else
    u32 u = 239 - Y, v = X;
#endif
    /* centre of view pixel u/v, as a fraction of the picture */
    s32 up = (s32)u - PT_IMG_X;         /* signed: PT_IMG_X may be 0 */
    if (up < 0)
        up = 0;
    if (up > PT_IMG_W - 1)
        up = PT_IMG_W - 1;
    *fx = ((u32)up * 2 + 1) * 32768u / PT_IMG_W;
    *fy = (v * 2 + 1) * 32768u / PT_VIEW_H;
}
