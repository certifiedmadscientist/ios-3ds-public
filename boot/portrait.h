#ifndef PORTRAIT_H
#define PORTRAIT_H

/*
 * Portrait view of the 320x480 iPhone panel on the 3DS bottom screen
 * (LCD=iphone ORIENT=portrait).
 *
 * The 3DS is held like a book: turned 90 degrees counter-clockwise, top
 * screen on the LEFT, touch screen on the right, D-pad / circle pad at the
 * bottom.  The bottom screen is then a 240-wide x 320-tall view (u, v),
 * u left to right, v top to bottom, and the physical screen in its normal
 * landscape coordinates (X 0..319 left to right, Y 0..239 top to bottom,
 * as the touch panel reports them) relates to it as
 *
 *   PORTRAIT_CCW 1 (book grip, top screen left):   X = 319 - v,  Y = u
 *   PORTRAIT_CCW 0 (turned clockwise, top right):  X = v,        Y = 239 - u
 *
 * Scale: 480 panel rows -> 320 view rows is 2/3.  With PORTRAIT_FILL 0 the
 * width uses the same 2/3 (320 -> 213 columns, centred, black bars of
 * 13/14 px), keeping the aspect ratio exact; PORTRAIT_FILL 1 stretches the
 * width 3/4 (320 -> 240) to fill the screen, 11% wider than true.
 *
 * Both axes use an area ("box") filter: every view pixel is the average of
 * the panel area it covers, which for ratios 2/3 and 3/4 touches at most
 * two panel pixels per axis.  Weights are rounded to eighths built from
 * halving adds (ARMv6 UHADD8, four channels per instruction): 3/4 and 1/2
 * exact, 2/3 as 5/8.
 */

#include "types.h"

#ifndef PORTRAIT_CCW
#define PORTRAIT_CCW    1       /* flip to 0 if the picture is upside down */
#endif
#ifndef PORTRAIT_FILL
#define PORTRAIT_FILL   0
#endif

#define PT_SRC_W        320     /* iPhone panel */
#define PT_SRC_H        480
#define PT_VIEW_W       240     /* portrait view on the bottom screen */
#define PT_VIEW_H       320
#define PT_IMG_W        (PORTRAIT_FILL ? 240 : 213)
#define PT_IMG_X        ((PT_VIEW_W - PT_IMG_W) / 2)

/* Build the filter tables (once, before the first pt_render_column). */
void pt_init(void);

/* Render bottom-screen column X (240 BGR8 pixels, stored bottom row first,
 * 180 words) from the 32bpp BGRA panel at src.  rowbuf: 320 words. */
void pt_render_column(volatile u32 *dstcol, const volatile u32 *src, u32 X, u32 *rowbuf);

/* Touch: bottom-screen point (X 0..319, Y 0..239) -> panel position as
 * 0..65535 fractions of its width / height (clamped to the picture). */
void pt_touch(u32 X, u32 Y, u32 *fx, u32 *fy);

/* View <-> screen, exposed for tests. */
void pt_view_to_screen(u32 u, u32 v, u32 *X, u32 *Y);

#endif
