/*
 * pl080.c — PL080 DMA controller emulation, after QEMU's hw/dma/pl080.c.
 *
 * Memory and NAND-FIFO channels run as soon as they are enabled (the NAND
 * FIFO is always ready), but only for a bounded slice per call: a linked
 * list that points back at itself never completes, so a channel that hits
 * the slice stays enabled and pl080_tick() (heartbeat) resumes it.  Channels
 * with any other
 * device endpoint (SPI data registers) are paced like peripheral flow
 * control: pl080_service() moves only what the device can take or holds
 * (periph_dma_avail), so a looping RX descriptor chain cannot run away.
 */

#include "pl080.h"
#include "nand.h"
#include "nand_hal.h"
#include "periph.h"

#define NOLDP __attribute__((optimize("no-tree-loop-distribute-patterns")))

#define NCHAN 8

#define INTSTATUS    0x000
#define INTTCSTATUS  0x004
#define INTTCCLEAR   0x008
#define INTERRSTATUS 0x00C
#define INTERRCLEAR  0x010
#define RAWTCSTATUS  0x014
#define RAWERRSTATUS 0x018
#define ENBLDCHNS    0x01C
#define CONFIG       0x030
#define SYNC         0x034

#define CCONF_E    (1u << 0)
#define CCONF_IE   (1u << 14)
#define CCONF_ITC  (1u << 15)
#define CCONF_A    (1u << 17)
#define CCONF_H    (1u << 18)
#define CCTRL_SI   (1u << 26)
#define CCTRL_DI   (1u << 27)
#define CCTRL_I    (1u << 31)

#define SLICE_LLI   64u          /* descriptors per call, then resume later */
#define SLICE_BYTES 0x40000u     /* bytes per enable (256 KB) */
#define TICK_BYTES  0x4000u      /* bytes per resume (heartbeat, poll) */

typedef struct {
    u32 src, dst, lli, ctrl, conf;
    u32 src0, dst0, moved0;      /* at enable: for nand_note_dma */
    bool started, capped;
} chan_t;

typedef struct {
    u32 conf, sync, tc_int, err_int;
    chan_t ch[NCHAN];
} dmac_t;

static dmac_t d[2];
static const int irq_line[2] = { 16, 17 };
static const u8 pl080_id[8] = { 0x80, 0x10, 0x04, 0x0A, 0x0D, 0xF0, 0x05, 0xB1 };
static u32 moved;
static u32 dmac1_moved, dmac1_done, dmac1_last_ch;

/* Breadcrumb: nibble stack of the emulation routines currently running
 * (see pl080.h).  Shown on status row 0 and in every cap message. */
volatile u32 htl_crumb;

/* One line, once per call site (the caller keeps the flag). */
void hang_cap(bool *shown, const char *what, u32 a, u32 b, u32 c, u32 e) {
    static const char hexd[] = "0123456789ABCDEF";
    if (*shown)
        return;
    *shown = true;
    char buf[64], *p = buf;
    while (*what && p < buf + 14)
        *p++ = *what++;
    u32 v[5] = { a, b, c, e, htl_crumb };
    static const char tag[5] = { ' ', ' ', ' ', ' ', 'K' };
    for (int k = 0; k < 5; k++) {
        *p++ = ' ';
        if (k == 4)
            *p++ = tag[k];
        for (int i = 7; i >= 0; i--)
            *p++ = hexd[(v[k] >> (4 * i)) & 0xF];
    }
    *p = 0;
    nhal_puts(buf);
}

static bool is_device(u32 pa) { return pa >= 0x38000000u && pa < 0x40000000u; }

/* Device endpoint that paces the channel (anything but the NAND FIFO). */
static bool paced(const chan_t *ch) {
    return (is_device(ch->src) && ch->src != NAND_FIFO_PA) ||
           (is_device(ch->dst) && ch->dst != NAND_FIFO_PA);
}

static u32 masks(const dmac_t *c, u32 bit) {
    u32 m = 0;
    for (int i = 0; i < NCHAN; i++)
        if (c->ch[i].conf & bit)
            m |= 1u << i;
    return m;
}

static u32 int_status(const dmac_t *c) {
    return (c->tc_int & masks(c, CCONF_ITC)) | (c->err_int & masks(c, CCONF_IE));
}

static void update_irq(int n) {
    nhal_irq(irq_line[n], int_status(&d[n]) != 0);
}

static u32 dev_read(u32 pa, u32 width) {
    if (pa == NAND_FIFO_PA)
        return nand_fifo_read_dma();
    return periph_dispatch(pa, 0, false, width);
}

static void dev_write(u32 pa, u32 v, u32 width) {
    if (pa == NAND_FIFO_PA)
        nand_fifo_write(v);
    else
        periph_dispatch(pa, v, true, width);
}

/* Contiguous memory writes are batched into one nhal_phys_write. */
static u8  wbuf[512] ALIGNED(4);
static u32 wbuf_pa, wbuf_len;

static void wflush(void) {
    if (wbuf_len)
        nhal_phys_write(wbuf_pa, wbuf, wbuf_len);
    wbuf_len = 0;
}

static void mem_put(u32 pa, const u8 *b, u32 n) {
    if (wbuf_len && (pa != wbuf_pa + wbuf_len || wbuf_len + n > sizeof(wbuf)))
        wflush();
    if (!wbuf_len)
        wbuf_pa = pa;
    for (u32 i = 0; i < n; i++)
        wbuf[wbuf_len + i] = b[i];
    wbuf_len += n;
}

static NOLDP void run_channel(dmac_t *c, chan_t *ch, u32 budget) {
    static bool shown;
    u8 fifo[8];
    u32 fill = 0;

    CRUMB_IN(2);
    if (!ch->started) {
        ch->started = true;
        ch->src0 = ch->src;
        ch->dst0 = ch->dst;
        ch->moved0 = moved;
    }
    ch->capped = false;

    for (u32 desc_n = 0; ; desc_n++) {
        u32 size = ch->ctrl & 0xFFF;
        u32 sw = 1u << ((ch->ctrl >> 18) & 7);
        u32 dw = 1u << ((ch->ctrl >> 21) & 7);
        if (sw > 4) sw = 4;
        if (dw > 4) dw = 4;

        if (desc_n >= SLICE_LLI || (size && !budget)) {
            ch->capped = true;
            hang_cap(&shown, "[DMA] cap ch", (u32)(ch - c->ch) | ((u32)(c - d) << 4),
                     ch->lli, ch->src, ch->dst);
            CRUMB_OUT();
            return;                     /* still enabled: pl080_tick resumes */
        }

        while (size && (budget || fill)) {
            u32 v = 0;
            if (is_device(ch->src)) {
                v = dev_read(ch->src, sw);
            } else {
                u8 b[4] = { 0, 0, 0, 0 };
                nhal_phys_read(ch->src, b, sw);
                v = b[0] | (b[1] << 8) | (b[2] << 16) | ((u32)b[3] << 24);
            }
            for (u32 i = 0; i < sw; i++)
                fifo[fill++] = v >> (8 * i);
            if (ch->ctrl & CCTRL_SI)
                ch->src += sw;
            size--;
            budget = budget > sw ? budget - sw : 0;

            while (fill >= dw) {
                if (is_device(ch->dst)) {
                    u32 w = 0;
                    for (u32 i = 0; i < dw; i++)
                        w |= (u32)fifo[i] << (8 * i);
                    dev_write(ch->dst, w, dw);
                } else {
                    mem_put(ch->dst, fifo, dw);
                }
                moved += dw;
                for (u32 i = dw; i < fill; i++)
                    fifo[i - dw] = fifo[i];
                fill -= dw;
                if (ch->ctrl & CCTRL_DI)
                    ch->dst += dw;
            }
        }
        wflush();
        ch->ctrl = (ch->ctrl & ~0xFFFu) | size;
        if (size)
            continue;                   /* out of budget: caps above */

        if (ch->lli & ~3u) {
            u8 desc[16];
            if (!nhal_phys_read(ch->lli & ~3u, desc, 16))
                break;
            u32 *w = (u32 *)desc;
            ch->src = w[0];
            ch->dst = w[1];
            ch->lli = w[2];
            ch->ctrl = w[3];
            continue;
        }
        break;
    }

    if (ch->src0 == NAND_FIFO_PA || ch->dst0 == NAND_FIFO_PA)
        nand_note_dma((u32)(ch - c->ch), ch->src0, ch->dst0, moved - ch->moved0);
    ch->started = false;
    ch->conf &= ~CCONF_E;
    if (ch->ctrl & CCTRL_I)
        c->tc_int |= 1u << (ch - c->ch);
    CRUMB_OUT();
}

/* Move at most budget bytes on the device side of a paced channel.
 * Completes blocks (terminal-count interrupt, next descriptor) as it goes. */
static NOLDP void run_paced(dmac_t *c, chan_t *ch, u32 budget) {
    static bool shown;
    bool to_dev = is_device(ch->dst) && ch->dst != NAND_FIFO_PA;
    CRUMB_IN(3);
    for (u32 desc_n = 0; ; desc_n++) {
        if (desc_n >= SLICE_LLI) {
            hang_cap(&shown, "[DMA] cap pc", (u32)(ch - c->ch) | ((u32)(c - d) << 4),
                     ch->lli, ch->src, ch->dst);
            CRUMB_OUT();
            return;                     /* still enabled: resumed later */
        }
        u32 size = ch->ctrl & 0xFFF;
        u32 sw = 1u << ((ch->ctrl >> 18) & 7);
        u32 dw = 1u << ((ch->ctrl >> 21) & 7);
        if (sw > 4) sw = 4;
        if (dw > 4) dw = 4;
        u32 unit = to_dev ? dw : sw;
        if (sw != dw)                       /* keep unit transfers simple */
            sw = dw = unit;

        while (size && budget >= unit) {
            u32 v;
            if (is_device(ch->src)) {
                v = dev_read(ch->src, sw);
            } else {
                u8 b[4] = { 0, 0, 0, 0 };
                nhal_phys_read(ch->src, b, sw);
                v = b[0] | (b[1] << 8) | (b[2] << 16) | ((u32)b[3] << 24);
            }
            if (is_device(ch->dst)) {
                dev_write(ch->dst, v, dw);
            } else {
                u8 b[4] = { (u8)v, (u8)(v >> 8), (u8)(v >> 16), (u8)(v >> 24) };
                mem_put(ch->dst, b, dw);
            }
            if (ch->ctrl & CCTRL_SI)
                ch->src += sw;
            if (ch->ctrl & CCTRL_DI)
                ch->dst += dw;
            moved += dw;
            if (c == &d[1])
                dmac1_moved += dw;
            budget -= unit;
            size--;
            ch->ctrl = (ch->ctrl & ~0xFFFu) | size;
        }
        wflush();
        if (size) {
            CRUMB_OUT();
            return;                         /* device not ready for more */
        }
        if (ch->ctrl & CCTRL_I)
            c->tc_int |= 1u << (ch - c->ch);
        if (ch->lli & ~3u) {
            u8 desc[16];
            if (!nhal_phys_read(ch->lli & ~3u, desc, 16))
                break;
            u32 *w = (u32 *)desc;
            ch->src = w[0];
            ch->dst = w[1];
            ch->lli = w[2];
            ch->ctrl = w[3];
            if (!budget) {
                CRUMB_OUT();
                return;
            }
            continue;
        }
        break;
    }
    ch->conf &= ~CCONF_E;
    if (c == &d[1])
        dmac1_done++;
    CRUMB_OUT();
}

static void run_all(int n) {
    dmac_t *c = &d[n];
    if (!(c->conf & 1))
        return;
    for (int i = 0; i < NCHAN; i++)
        if ((c->ch[i].conf & CCONF_E) && !paced(&c->ch[i]))
            run_channel(c, &c->ch[i], SLICE_BYTES);
    update_irq(n);
    pl080_service();
}

/* Heartbeat: resume channels that hit their slice, and paced channels. */
void pl080_tick(void) {
    for (int n = 0; n < 2; n++) {
        dmac_t *c = &d[n];
        if (!(c->conf & 1))
            continue;
        bool any = false;
        for (int i = 0; i < NCHAN; i++)
            if ((c->ch[i].conf & CCONF_E) && c->ch[i].capped && !paced(&c->ch[i])) {
                run_channel(c, &c->ch[i], TICK_BYTES);
                any = true;
            }
        if (any)
            update_irq(n);
    }
    pl080_service();
}

/* Service every enabled paced channel on both controllers with what its
 * device can currently take or provide. */
void pl080_service(void) {
    static bool busy;
    if (busy)
        return;
    busy = true;
    for (int n = 0; n < 2; n++) {
        dmac_t *c = &d[n];
        if (!(c->conf & 1))
            continue;
        bool any = false;
        for (int i = 0; i < NCHAN; i++) {
            chan_t *ch = &c->ch[i];
            if (!(ch->conf & CCONF_E) || (ch->conf & CCONF_H) || !paced(ch))
                continue;
            bool to_dev = is_device(ch->dst) && ch->dst != NAND_FIFO_PA;
            u32 avail = periph_dma_avail(to_dev ? ch->dst : ch->src, to_dev);
            if (avail) {
                run_paced(c, ch, avail);
                any = true;
            }
        }
        if (any)
            update_irq(n);
    }
    busy = false;
}

u32 pl080_read(int n, u32 off) {
    dmac_t *c = &d[n & 1];
    /* A driver polling with IRQs masked must still see progress. */
    pl080_service();
    for (int i = 0; i < NCHAN; i++)
        if ((c->ch[i].conf & CCONF_E) && c->ch[i].capped && (c->conf & 1)) {
            run_channel(c, &c->ch[i], TICK_BYTES);
            update_irq(n & 1);
        }
    if (off >= 0xFE0 && off < 0x1000)
        return pl080_id[(off - 0xFE0) >> 2];
    if (off >= 0x100 && off < 0x100 + NCHAN * 0x20) {
        chan_t *ch = &c->ch[(off - 0x100) >> 5];
        switch (off & 0x1F) {
        case 0x00: return ch->src;
        case 0x04: return ch->dst;
        case 0x08: return ch->lli;
        case 0x0C: return ch->ctrl;
        case 0x10: {
            /* A (bit 17) is read-only: data in the channel FIFO.  Only a
             * memory/NAND channel cut off by a slice cap holds any; a paced
             * channel waiting for its device reads idle (the PL080 driver
             * spins on A in queryDMACommand / halt, c033d870, c033dfc0). */
            u32 v = ch->conf & ~CCONF_A;
            if ((v & CCONF_E) && !(v & CCONF_H) && ch->capped && !paced(ch))
                v |= CCONF_A;
            return v;
        }
        default:   return 0;
        }
    }
    switch (off) {
    case INTSTATUS:    return int_status(c);
    case INTTCSTATUS:  return c->tc_int & masks(c, CCONF_ITC);
    case INTERRSTATUS: return c->err_int & masks(c, CCONF_IE);
    case RAWTCSTATUS:  return c->tc_int;
    case RAWERRSTATUS: return c->err_int;
    case ENBLDCHNS:    return masks(c, CCONF_E);
    case CONFIG:       return c->conf;
    case SYNC:         return c->sync;
    default:           return 0;
    }
}

void pl080_write(int n, u32 off, u32 val) {
    n &= 1;
    dmac_t *c = &d[n];
    if (off >= 0x100 && off < 0x100 + NCHAN * 0x20) {
        chan_t *ch = &c->ch[(off - 0x100) >> 5];
        switch (off & 0x1F) {
        case 0x00: ch->src = val; break;
        case 0x04: ch->dst = val; break;
        case 0x08: ch->lli = val; break;
        case 0x0C: ch->ctrl = val; break;
        case 0x10:
            if (!(ch->conf & CCONF_E) || !(val & CCONF_E))
                ch->started = ch->capped = false;
            if (n == 1 && (val & CCONF_E))
                dmac1_last_ch = (u32)(ch - c->ch);
            ch->conf = val & ~CCONF_A;
            run_all(n);
            break;
        default: break;
        }
        return;
    }
    switch (off) {
    case INTTCCLEAR:  c->tc_int &= ~val;  update_irq(n); break;
    case INTERRCLEAR: c->err_int &= ~val; update_irq(n); break;
    case CONFIG:      c->conf = val; run_all(n); break;
    case SYNC:        c->sync = val; break;
    default: break;
    }
}

void pl080_init(void) {
    for (int n = 0; n < 2; n++) {
        d[n].conf = d[n].sync = d[n].tc_int = d[n].err_int = 0;
        for (int i = 0; i < NCHAN; i++)
            d[n].ch[i].src = d[n].ch[i].dst = d[n].ch[i].lli =
            d[n].ch[i].ctrl = d[n].ch[i].conf = 0,
            d[n].ch[i].started = d[n].ch[i].capped = false;
    }
}

u32 pl080_bytes(void) { return moved; }

/* Current level of a DMAC's interrupt output (for stall dumps). */
u32 nand_dma_irq_level(int n) {
    return int_status(&d[n & 1]) != 0;
}

/* "Q1 e<enabled> t<raw TC> q<irq> n<bytes> c<channels done> k<ch> L<lli> z<ctrl>"
 * for dmac1 (SPI2's DMA parent), the last channel it enabled. */
void pl080_status_line(char *buf) {
    static const char hx[] = "0123456789ABCDEF";
    char *p = buf;
    const dmac_t *c = &d[1];
    const chan_t *ch = &c->ch[dmac1_last_ch & 7];
#define PUT(tag, v, nd) do { *p++ = ' '; *p++ = tag; \
        for (int i_ = (nd) - 1; i_ >= 0; i_--) *p++ = hx[((v) >> (4 * i_)) & 0xF]; } while (0)
    *p++ = 'Q'; *p++ = '1';
    PUT('e', masks(c, CCONF_E), 2);
    PUT('t', c->tc_int, 2);
    PUT('q', int_status(c) != 0, 1);
    PUT('n', dmac1_moved, 5);
    PUT('c', dmac1_done, 3);
    PUT('k', dmac1_last_ch, 1);
    PUT('L', ch->lli, 8);
    PUT('z', ch->ctrl, 8);
#undef PUT
    *p = 0;
}

/* An enabled channel still writes to the device register at pa (an SPI
 * TXDATA): the transfer isn't over just because the FIFO ran dry. */
bool pl080_feeds(u32 pa) {
    for (int n = 0; n < 2; n++) {
        if (!(d[n].conf & 1))
            continue;
        for (int i = 0; i < NCHAN; i++)
            if ((d[n].ch[i].conf & CCONF_E) && !(d[n].ch[i].conf & CCONF_H) &&
                d[n].ch[i].dst == pa)
                return true;
    }
    return false;
}
