/*
 * S5L8900 UART emulation.
 * UART0 at 0x3CC00000, spaced 0x4000 apart.
 *
 * Redirects TX output to the 3DS top-screen debug console.
 *
 * Reference: Samsung S3C-style UART registers, QEMU-iOS (devos50/qemu-ios)
 */

#include "uart.h"
#include "../console.h"

/* Register offsets */
#define ULCON    0x000  /* Line control */
#define UCON     0x004  /* Control */
#define UFCON    0x008  /* FIFO control */
#define UMCON    0x00C  /* Modem control */
#define UTRSTAT  0x010  /* TX/RX status */
#define UERSTAT  0x014  /* Error status */
#define UFSTAT   0x018  /* FIFO status */
#define UMSTAT   0x01C  /* Modem status */
#define UTXH     0x020  /* TX holding (write byte to transmit) */
#define URXH     0x024  /* RX holding (read received byte) */
#define UBRDIV   0x028  /* Baud rate divisor */

/* Number of UARTs */
#define NUM_UARTS 5

/* UTRSTAT bits */
#define UTRSTAT_RX_READY   (1 << 0)
#define UTRSTAT_TX_EMPTY   (1 << 1)
#define UTRSTAT_TX_DONE    (1 << 2)

/* Per-UART register state */
typedef struct {
    u32 ulcon;
    u32 ucon;
    u32 ufcon;
    u32 umcon;
    u32 ubrdiv;
} uart_state_t;

static uart_state_t uarts[NUM_UARTS];
static u32 tx_count[NUM_UARTS];

/* ── Kernel console tap ───────────────────────────────────────────────
 * The kernel's UART0 text is gathered line by line before it reaches the
 * console.  With the io=0x6 boot argument IOKit prints a line for every
 * probe ("X::probe(Y)") and start ("X::start(Y) <n>", plus "failed"); those
 * would flood the wrapping log, so they are only counted, and the last
 * failed start and the last display-related line are kept for the pinned
 * status rows.  Everything else is passed on unchanged. */

#define TAP_LINE 100

static char tap_buf[TAP_LINE + 1];
static u32 tap_len, tap_seen;
static u32 n_probe, n_start, n_fail;
static char last_fail[64], last_disp[64];

static bool tap_has(const char *s, const char *needle) {
    for (; *s; s++) {
        const char *a = s, *b = needle;
        while (*a && *b && *a == *b) {
            a++;
            b++;
        }
        if (!*b)
            return true;
    }
    return false;
}

/* Copy for a status row: printable ASCII only; "Apple", "ARM" and "Device"
 * dropped to save columns; a failed start's "<n> failed" tail becomes FAIL. */
static bool tap_at(const char *s, const char *w) {
    while (*w && *s == *w) {
        s++;
        w++;
    }
    return !*w;
}

static void tap_keep(char *dst, const char *s) {
    u32 n = 0;
    while (*s && n < 58) {
        if (s[0] == ' ' && s[1] == '<' && tap_has(s, "failed")) {
            const char *f = " FAIL";
            while (*f && n < 63)
                dst[n++] = *f++;
            break;
        }
        if (tap_at(s, "Apple")) { s += 5; continue; }
        if (tap_at(s, "ARM"))   { s += 3; continue; }
        if (tap_at(s, "Device")) { s += 6; continue; }
        char c = *s++;
        if (c >= 0x20 && c < 0x7F)
            dst[n++] = c;
    }
    dst[n] = 0;
}

/* Per-node driver state from the IOKit trace: the provider's name is the
 * text inside "::start(...)" / "::probe(...)".  '.' none, 'P' probed,
 * 'S' started, 'F' start failed. */
static const struct { const char *node; const char *label; } nodes[16] = {
    { "i2c0", "i2c0" }, { "i2c1", "i2c1" }, { "spi0", "spi0" }, { "spi1", "spi1" },
    { "pmu", "pmu" },   { "lcd0", "lcd0" }, { "clcd", "clcd" }, { "gpio", "gpio" },
    { "power", "powr" }, { "clkrstgen", "clkr" }, { "timer", "timr" }, { "vic", "vic" },
    { "uart0", "uar0" }, { "aes", "aes" },   { "sha1", "sha1" }, { "chipid", "chip" },
};
static u8 node_state[16];

static void tap_node(const char *line, bool start, bool failed) {
    const char *p = line;
    while (*p && !tap_at(p, start ? "::start(" : "::probe("))
        p++;
    if (!*p)
        return;
    p += 8;
    char name[16];
    u32 n = 0;
    while (*p && *p != ')' && n < 15)
        name[n++] = *p++;
    name[n] = 0;
    for (u32 i = 0; i < 16; i++) {
        const char *a = name, *b = nodes[i].node;
        while (*a && *a == *b) {
            a++;
            b++;
        }
        if (*a || *b)
            continue;
        if (start)
            node_state[i] = failed ? 3 : (node_state[i] == 3 ? 3 : 2);
        else if (!node_state[i])
            node_state[i] = 1;
        return;
    }
}

static char last_panic[64];      /* last UART line mentioning a panic */

/* For the spin report: the last kernel line with "panic"/"Debugger". */
void uart_tap_panic_line(char *out, u32 max)
{
    u32 i = 0;
    for (; last_panic[i] && i + 1 < max; i++)
        out[i] = last_panic[i];
    out[i] = 0;
}

static void tap_line(void) {
    tap_buf[tap_len] = 0;
    if (tap_has(tap_buf, "panic") || tap_has(tap_buf, "Debugger"))
        tap_keep(last_panic, tap_buf);
    bool start = tap_has(tap_buf, "::start(");
    bool probe = tap_has(tap_buf, "::probe(");

    static const char *const disp[] = {
        "CLCD", "clcd", "LCD", "lcd", "Merlot", "MPL", "mpl", "Backlight",
        "backlight", "IOMobile", "CoreSurface", "Framebuffer", "MBX", "GPIO",
        "gpio", "PMU", "pmu", "PCF", "I2C", "i2c", "SPI", "spi",
    };
    for (u32 i = 0; i < sizeof(disp) / sizeof(disp[0]); i++)
        if (tap_has(tap_buf, disp[i])) {
            tap_keep(last_disp, tap_buf);
            break;
        }

    if (start || probe) {
        tap_node(tap_buf, start, start && tap_has(tap_buf, "failed"));
        if (start) {
            n_start++;
            if (tap_has(tap_buf, "failed")) {
                n_fail++;
                tap_keep(last_fail, tap_buf);
            }
        } else {
            n_probe++;
        }
    } else {
        for (u32 i = 0; i < tap_len; i++)
            console_putc(tap_buf[i]);
    }
    tap_len = 0;
}

static void tap_putc(char c) {
    tap_buf[tap_len++] = c;
    tap_seen++;
    if (c == '\n' || tap_len >= TAP_LINE)
        tap_line();
}

/* Called from the heartbeat: a partial line that has stopped growing (a
 * prompt, or output cut off by a hang) is released to the console. */
void uart_tap_idle(void)
{
    static u32 last_seen;
    if (tap_len && tap_seen == last_seen) {
        tap_buf[tap_len] = 0;
        for (u32 i = 0; i < tap_len; i++)
            console_putc(tap_buf[i]);
        tap_len = 0;
    }
    last_seen = tap_seen;
}

static char *put_hex_n(char *p, u32 v, u32 digits)
{
    static const char h[] = "0123456789ABCDEF";
    for (int i = (int)digits - 1; i >= 0; i--)
        *p++ = h[(v >> (i * 4)) & 0xF];
    return p;
}

/* Two rows of node states, 8 per row: "i2c0S i2c1S spi0. ...". */
void uart_tap_nodes(char *row_a, char *row_b)
{
    static const char mark[4] = { '.', 'P', 'S', 'F' };
    for (u32 r = 0; r < 2; r++) {
        char *p = r ? row_b : row_a;
        for (u32 i = r * 8; i < r * 8 + 8; i++) {
            if (i != r * 8)
                *p++ = ' ';
            for (const char *l = nodes[i].label; *l; l++)
                *p++ = *l;
            *p++ = mark[node_state[i]];
        }
        *p = 0;
    }
}

/* Rows: counters + last failed start; last display-related kernel line. */
void uart_tap_rows(char *row_a, char *row_b)
{
    char *p = row_a;
    *p++ = 'p'; p = put_hex_n(p, n_probe, 4);
    *p++ = ' '; *p++ = 's'; p = put_hex_n(p, n_start, 4);
    *p++ = ' '; *p++ = 'f'; p = put_hex_n(p, n_fail, 3);
    *p++ = ' ';
    for (u32 i = 0; last_fail[i] && p - row_a < 49; i++)
        *p++ = last_fail[i];
    *p = 0;

    p = row_b;
    for (u32 i = 0; last_disp[i] && i < 49; i++)
        *p++ = last_disp[i];
    *p = 0;
}

void uart_tx_counts(u32 *c)
{
    for (int i = 0; i < NUM_UARTS; i++)
        c[i] = tx_count[i];
}

void uart_init(void)
{
    for (int i = 0; i < NUM_UARTS; i++) {
        uarts[i].ulcon = 0;
        uarts[i].ucon = 0;
        uarts[i].ufcon = 0;
        uarts[i].umcon = 0;
        uarts[i].ubrdiv = 0;
    }
}

u32 uart_read(int uart_index, u32 offset)
{
    if (uart_index < 0 || uart_index >= NUM_UARTS)
        return 0;

    uart_state_t *u = &uarts[uart_index];

    switch (offset) {
    case ULCON:
        return u->ulcon;
    case UCON:
        return u->ucon;
    case UFCON:
        return u->ufcon;
    case UMCON:
        return u->umcon;

    case UTRSTAT:
        /*
         * Always report TX buffer empty and transmitter done.
         * No RX data available.
         */
        return UTRSTAT_TX_EMPTY | UTRSTAT_TX_DONE;

    case UERSTAT:
        return 0;  /* No errors */

    case UFSTAT:
        return 0;  /* FIFOs empty */

    case UMSTAT:
        return 0;  /* No modem status */

    case URXH:
        return 0;  /* No received data */

    case UBRDIV:
        return u->ubrdiv;

    default:
        return 0;
    }
}

void uart_write(int uart_index, u32 offset, u32 value)
{
    if (uart_index < 0 || uart_index >= NUM_UARTS)
        return;

    uart_state_t *u = &uarts[uart_index];

    switch (offset) {
    case ULCON:
        u->ulcon = value;
        break;
    case UCON:
        u->ucon = value;
        break;
    case UFCON:
        u->ufcon = value;
        break;
    case UMCON:
        u->umcon = value;
        break;

    case UTXH:
        tx_count[uart_index]++;
        /*
         * Transmit a character. Send UART0 output to the 3DS debug
         * console; silently discard output from other UARTs.
         */
        if (uart_index == 0)
            tap_putc((char)(value & 0xFF));
        break;

    case UBRDIV:
        u->ubrdiv = value;
        break;

    default:
        break;
    }
}
