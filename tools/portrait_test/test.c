/*
 * Host test of boot/portrait.c: view <-> screen mapping at the corners and
 * centre, touch -> panel against what is actually drawn there, letterbox
 * bars, and the area filter (flat colour exact, 1-px checkerboard -> grey,
 * 1-px lines never dropped).  Built for PORTRAIT_CCW 0/1 x PORTRAIT_FILL
 * 0/1 by run.sh.
 */
#include <stdio.h>
#include <stdint.h>
#include <stdlib.h>
typedef uint32_t u32; typedef uint8_t u8;
void pt_init(void);
void pt_render_column(volatile u32 *dstcol, const volatile u32 *src, u32 X, u32 *rowbuf);
void pt_touch(u32 X, u32 Y, u32 *fx, u32 *fy);
void pt_view_to_screen(u32 u, u32 v, u32 *X, u32 *Y);

#define SW 320
#define SH 480
static u32 panel[SW * SH], screen[320 * 180], rowbuf[SW];
static int fails;
#define CHECK(c, ...) do { if (!(c)) { fails++; printf("FAIL: " __VA_ARGS__); printf("\n"); } } while (0)

static void render(void) { for (u32 X = 0; X < 320; X++) pt_render_column(screen + X * 180, panel, X, rowbuf); }
static u32 at(u32 X, u32 Y) {          /* BGR of screen pixel (X, Y) */
    const u8 *c = (const u8 *)(screen + X * 180);
    u32 i = 239 - Y;
    return c[3 * i] | c[3 * i + 1] << 8 | c[3 * i + 2] << 16;
}
static u32 rgb(u32 r, u32 g, u32 b) { return 0xFF000000u | r << 16 | g << 8 | b; }

int main(void) {
    pt_init();
    int img_w = PORTRAIT_FILL ? 240 : 213, img_x = (240 - img_w) / 2;

    /* 1. corners and centre of the view */
    static const struct { u32 u, v; const char *name; } pts[] = {
        { 0, 0, "view top-left" }, { 239, 0, "view top-right" },
        { 0, 319, "view bottom-left" }, { 239, 319, "view bottom-right" }, { 120, 160, "centre" } };
    for (int k = 0; k < 5; k++) {
        u32 X, Y; pt_view_to_screen(pts[k].u, pts[k].v, &X, &Y);
        u32 eX = PORTRAIT_CCW ? 319 - pts[k].v : pts[k].v, eY = PORTRAIT_CCW ? pts[k].u : 239 - pts[k].u;
        printf("  %-18s view (%3u,%3u) -> screen (%3u,%3u)\n", pts[k].name, pts[k].u, pts[k].v, X, Y);
        CHECK(X == eX && Y == eY, "%s", pts[k].name);
    }

    /* 2. orientation: panel top rows red, left columns green */
    for (u32 y = 0; y < SH; y++) for (u32 x = 0; x < SW; x++)
        panel[y * SW + x] = y < 24 ? rgb(255, 0, 0) : x < 16 ? rgb(0, 255, 0) : rgb(0, 0, 0);
    render();
    {
        u32 X, Y; pt_view_to_screen(120, 2, &X, &Y);
        CHECK(at(X, Y) == 0xFF0000, "panel top at view top (screen %u,%u = %06x)", X, Y, at(X, Y));
        pt_view_to_screen(img_x + 2, 160, &X, &Y);
        CHECK(at(X, Y) == 0x00FF00, "panel left at view left (screen %u,%u = %06x)", X, Y, at(X, Y));
        if (img_x) { pt_view_to_screen(1, 160, &X, &Y); CHECK(at(X, Y) == 0, "letterbox bar black"); }
        printf("  panel top -> screen X %s, panel left -> screen Y %s\n",
               PORTRAIT_CCW ? "319 (right edge in landscape)" : "0", PORTRAIT_CCW ? "0" : "239");
    }

    /* 3. touch lands on what is drawn: panel encodes its own x, y */
    for (u32 y = 0; y < SH; y++) for (u32 x = 0; x < SW; x++)
        panel[y * SW + x] = rgb(y / 2, 0, x * 255 / 319);    /* linear ramps */
    render();
    int worst = 0;
    for (u32 X = 0; X < 320; X += 7) for (u32 Y = 0; Y < 240; Y += 5) {
        u32 u = PORTRAIT_CCW ? Y : 239 - Y;
        if (u < (u32)img_x || u >= (u32)(img_x + img_w)) continue;
        u32 fx, fy; pt_touch(X, Y, &fx, &fy);
        int tx = fx * SW >> 16, ty = fy * SH >> 16;
        u32 c = at(X, Y);           /* filtered, so the code is approximate */
        int dy = (c >> 16) * 2, dx = (c & 0xFF) * 319 / 255;
        int e = abs(tx - dx) > abs(ty - dy) ? abs(tx - dx) : abs(ty - dy);
        if (e > worst) worst = e;
    }
    printf("  touch vs drawn: worst error %d panel px\n", worst);
    CHECK(worst <= 3, "touch error %d px", worst);   /* ramp quantisation ~2 px */
    {
        static const u32 S[5][2] = { { 0, 0 }, { 319, 0 }, { 0, 239 }, { 319, 239 }, { 160, 120 } };
        for (int k = 0; k < 5; k++) {
            u32 fx, fy; pt_touch(S[k][0], S[k][1], &fx, &fy);
            printf("  tap screen (%3u,%3u) -> panel (%3u,%3u)\n", S[k][0], S[k][1], fx * SW >> 16, fy * SH >> 16);
        }
        u32 fx, fy; pt_touch(160, 120, &fx, &fy);
        CHECK(abs((int)(fx * SW >> 16) - 160) <= 2 && abs((int)(fy * SH >> 16) - 240) <= 2, "centre tap -> panel centre");
    }

    /* 4. filter: flat colour exact */
    for (u32 i = 0; i < SW * SH; i++) panel[i] = rgb(0x12, 0x9A, 0xEF);
    render();
    int bad = 0;
    for (u32 X = 0; X < 320; X++) for (u32 Y = 0; Y < 240; Y++) {
        u32 u = PORTRAIT_CCW ? Y : 239 - Y;
        if (u >= (u32)img_x && u < (u32)(img_x + img_w) && at(X, Y) != 0x129AEF) bad++;
    }
    CHECK(!bad, "flat colour changed at %d pixels", bad);

    /* 1-px checkerboard -> grey, not black/white */
    for (u32 y = 0; y < SH; y++) for (u32 x = 0; x < SW; x++)
        panel[y * SW + x] = (x ^ y) & 1 ? rgb(255, 255, 255) : rgb(0, 0, 0);
    render();
    int lo = 255, hi = 0;
    for (u32 X = 0; X < 320; X++) for (u32 Y = 0; Y < 240; Y++) {
        u32 u = PORTRAIT_CCW ? Y : 239 - Y;
        if (u < (u32)img_x || u >= (u32)(img_x + img_w)) continue;
        int g = (at(X, Y) >> 8) & 0xFF;
        if (g < lo) lo = g; if (g > hi) hi = g;
    }
    printf("  checkerboard -> grey %d..%d\n", lo, hi);
    CHECK(lo >= 64 && hi <= 192, "checkerboard range %d..%d", lo, hi);

    /* every 1-px horizontal and vertical panel line stays visible */
    int lost = 0;
    for (u32 L = 0; L < SH; L += 1) {
        for (u32 i = 0; i < SW * SH; i++) panel[i] = rgb(0, 0, 0);
        for (u32 x = 0; x < SW; x++) panel[L * SW + x] = rgb(255, 255, 255);
        u32 v = L * 320 / SH, X, Y, best = 0;
        for (int dv = -1; dv <= 1; dv++) {
            if ((int)v + dv < 0 || v + dv > 319) continue;
            pt_view_to_screen(120, v + dv, &X, &Y);
            pt_render_column(screen + X * 180, panel, X, rowbuf);
            u32 g = (at(X, Y) >> 8) & 0xFF; if (g > best) best = g;
        }
        if (best < 64) lost++;
    }
    for (u32 L = 0; L < SW; L++) {
        for (u32 i = 0; i < SW * SH; i++) panel[i] = rgb(0, 0, 0);
        for (u32 y = 0; y < SH; y++) panel[y * SW + L] = rgb(255, 255, 255);
        u32 X, Y; pt_view_to_screen(img_x, 160, &X, &Y);
        pt_render_column(screen + X * 180, panel, X, rowbuf);
        u32 u0 = img_x + L * img_w / SW, best = 0;
        for (int du = -1; du <= 1; du++) {
            u32 uu = u0 + du, XX, YY;
            if (uu < (u32)img_x || uu >= (u32)(img_x + img_w)) continue;
            pt_view_to_screen(uu, 160, &XX, &YY);
            u32 g = (at(XX, YY) >> 8) & 0xFF; if (g > best) best = g;
        }
        if (best < 64) lost++;
    }
    printf("  1-px lines lost: %d of %d\n", lost, SH + SW);
    CHECK(!lost, "%d one-pixel lines vanished", lost);

    printf(fails ? "RESULT: %d failure(s)\n" : "RESULT: all checks passed\n", fails);
    return fails != 0;
}
