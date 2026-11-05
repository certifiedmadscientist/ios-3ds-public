#include "console.h"
#include "screen.h"
#include "font.h"
#include "3ds_hw.h"

/* Console state */
#define CON_COLS  (SCREEN_TOP_WIDTH / FONT_WIDTH)   /* 400/8 = 50 */
#define CON_ROWS  (SCREEN_TOP_HEIGHT / FONT_HEIGHT) /* 240/8 = 30 */

static u32 con_col;
static u32 con_row;
static bool con_wrap;   /* wrap to row 0 instead of scrolling */

/* In wrap mode the last CON_STATUS_ROWS rows belong to console_status(). */
#define CON_STATUS_ROWS 16
#define CON_LOG_ROWS    (CON_ROWS - CON_STATUS_ROWS)

/* Colors: white text on black background */
#define TEXT_R 255
#define TEXT_G 255
#define TEXT_B 255

/* ── Internal helpers ────────────────────────────────────────────────── */

static void draw_char(u32 cx, u32 cy, char c) {
    const u8 *glyph = font_glyph(c);
    if (!glyph)
        return;

    u32 px = cx * FONT_WIDTH;
    u32 py = cy * FONT_HEIGHT;

    for (u32 row = 0; row < FONT_HEIGHT; row++) {
        u8 bits = glyph[row];
        for (u32 col = 0; col < FONT_WIDTH; col++) {
            if (bits & (0x80 >> col)) {
                screen_top_pixel(px + col, py + row, TEXT_R, TEXT_G, TEXT_B);
            }
            /* Background is already black from screen_init */
        }
    }
}

static void scroll_up(void) {
    /*
     * Scroll the top screen up by one text row (8 pixels).
     * The framebuffer is column-major, so we need to copy pixel by pixel
     * or work column-by-column.
     *
     * For each column x, the pixels are stored contiguously as a column:
     *   byte offset for (x, y) = ((239-y) + x*240) * 3
     * So within a column, adjacent y values are adjacent in memory
     * (but with y reversed: y=0 is at offset 239*3 within the column).
     *
     * To scroll up by 8 pixels: for each column, shift the column data
     * so that what was at y becomes what's at y-8.
     */
    u8 *fb = screen_top_fb();

    for (u32 x = 0; x < SCREEN_TOP_WIDTH; x++) {
        /* Base offset for this column in the framebuffer */
        u32 col_base = x * SCREEN_TOP_HEIGHT * FB_BPP;

        /* Within a column, pixel y is at offset (239-y)*3 from col_base.
           Scrolling up by 8: pixel at y gets the value from y+8.
           y+8 is at offset (239-(y+8))*3 = (231-y)*3
           y   is at offset (239-y)*3
           So we copy from (231-y)*3 to (239-y)*3.

           In terms of the raw column buffer (indexed by row position r,
           where r = 239-y, so r goes from 0..239):
             r corresponds to y = 239-r
             source y+8 has r' = 239-(y+8) = r-8
           So copy from r-8 to r, for r = 8..239.
           Then clear r = 0..7 (those are y = 239..232, the bottom 8 pixels,
           but in our rotated frame that's y=232..239 — the last 8 rows).
         */

        /* Copy: shift rows up by 8 pixels */
        for (u32 r = SCREEN_TOP_HEIGHT - 1; r >= 8; r--) {
            u32 dst = col_base + r * FB_BPP;
            u32 src = col_base + (r - 8) * FB_BPP;
            fb[dst + 0] = fb[src + 0];
            fb[dst + 1] = fb[src + 1];
            fb[dst + 2] = fb[src + 2];
        }

        /* Clear the top 8 rows (r = 0..7, which is y = 239..232) */
        /* Wait — we need to clear the BOTTOM text row, which is large y.
           Actually, scroll up means top rows get overwritten. Let me re-think.

           "Scroll up" = move text content upward. Row 1 goes to row 0, etc.
           The last row becomes empty.

           In pixel terms: pixel(x, y) gets pixel(x, y+8). The bottom 8 rows
           (y = 232..239) get cleared.

           y+8 row source has r_src = 239-(y+8) = 239-y-8 = r-8
           destination row r = 239-y

           For y=0..231 (valid source range): r=239..8
           Copy fb[col + (r-8)*3] -> fb[col + r*3]
           This must go from HIGH r to LOW r to avoid overwriting source.

           Actually no: r goes 239 down to 8. Source is r-8 (231 down to 0).
           Since r > r-8, we're reading from lower addresses and writing to
           higher, so direction doesn't matter for correctness here.

           Then clear y=232..239: r=7..0.
        */
        for (u32 r = 0; r < 8; r++) {
            u32 off = col_base + r * FB_BPP;
            fb[off + 0] = 0;
            fb[off + 1] = 0;
            fb[off + 2] = 0;
        }
    }
}

static void clear_row(u32 row) {
    for (u32 y = row * FONT_HEIGHT; y < (row + 1) * FONT_HEIGHT; y++)
        for (u32 x = 0; x < SCREEN_TOP_WIDTH; x++)
            screen_top_pixel(x, y, 0, 0, 0);
}

static void newline(void) {
    con_col = 0;
    con_row++;
    if (con_wrap) {
        if (con_row >= CON_LOG_ROWS)
            con_row = 0;
        clear_row(con_row);
        return;
    }
    if (con_row >= CON_ROWS) {
        scroll_up();
        con_row = CON_ROWS - 1;
    }
}

static void put_char(char c) {
    if (c == '\n') {
        newline();
        return;
    }

    if (con_col >= CON_COLS) {
        newline();
    }

    draw_char(con_col, con_row, c);
    con_col++;
}

/* ── Public API ──────────────────────────────────────────────────────── */

void console_init(void) {
    con_col = 0;
    con_row = 0;
}

void console_home(void) {
    screen_top_fill(0, 0, 0);
    con_col = 0;
    con_row = 0;
    con_wrap = true;
}

void console_status(u32 line, const char *s) {
    if (line >= CON_STATUS_ROWS)
        return;
    u32 row = CON_LOG_ROWS + line;
    clear_row(row);
    for (u32 col = 0; *s && col < CON_COLS; col++, s++)
        draw_char(col, row, *s);
}

void console_putc(char c) {
    put_char(c);
}

void console_print(const char *str) {
    while (*str) {
        put_char(*str);
        str++;
    }
}

void console_print_hex(u32 val) {
    static const char hex[] = "0123456789ABCDEF";
    char buf[11];
    buf[0] = '0';
    buf[1] = 'x';
    for (int i = 7; i >= 0; i--) {
        buf[2 + (7 - i)] = hex[(val >> (i * 4)) & 0xF];
    }
    buf[10] = '\0';
    console_print(buf);
}
