/*
 * S5L8900 peripheral dispatch.
 *
 * Routes trapped IO accesses from the HTL to the correct peripheral
 * emulator by physical address.  Addresses follow Apple's n45ap device
 * tree (arm-io base 0x38000000); behaviour follows qemu-ios.
 *
 *   0x38100000  Clock controller 0
 *   0x38000000  SHA-1 engine
 *   0x38C00000  AES engine
 *   0x38E00000  VIC0 (IRQs 0-31)
 *   0x38E01000  VIC1 (IRQs 32-63)
 *   0x39A00000  System IC / power
 *   0x3C300000  SPI0      0x3CE00000  SPI1      0x3D200000  SPI2
 *   0x3C500000  Clock controller 1
 *   0x3C600000  I2C0      0x3C900000  I2C1 (PMU at 0x73)
 *   0x3CC00000  UART0-4 (spaced 0x4000 apart)
 *   0x3E200000  Timer (64-bit timebase + deadline timer)
 *   0x3E400000  GPIO
 *   0x3E500000  Chip ID
 *   0x38200000  PL080 DMAC0   0x39900000  PL080 DMAC1
 *   0x38800000  ADM   0x38A00000  NAND FMC   0x38F00000  RS-ECC
 */

#include "periph.h"
#include "../display.h"
#include "vic.h"
#include "timer.h"
#include "uart.h"
#include "chipid.h"
#include "gpio.h"
#include "clock.h"
#include "sysic.h"
#include "i2c.h"
#include "aes.h"
#include "sha1.h"
#include "nand.h"
#include "pl080.h"
#include "input.h"
#include "../console.h"

void periph_init(void)
{
    vic_init();
    timer_init();
    uart_init();
    chipid_init();
    gpio_init();
    clock_init();
    sysic_init();
    i2c_init();
    aes_init();
    sha1_init();
    nand_init();
    pl080_init();
    console_print("HTL: periph init done\n");
}

/* ── SPI0-2 (qemu-ios hw/arm/ipod_touch_spi.c) ───────────────────────────
 * TX/RX byte FIFOs; while CTRL.RUN is set, each TX byte is clocked through
 * the attached device and, while RXCNT > 0, its reply lands in the RX
 * FIFO (with CFG.AGD, 0xFF is clocked out to fetch the rest).  SPI1 has
 * the Merlot LCD panel; SPI0 (LM2512 serializer) and SPI2 (multitouch,
 * not in our tree) answer 0.  STATUS is write-1-to-clear; the interrupt
 * line follows STATUS & the CFG interrupt-enable bits. */
#define SPI_CTRL        0x000
#define SPI_CTRL_RUN    (1u << 0)
#define SPI_CTRL_TXRST  (1u << 2)
#define SPI_CTRL_RXRST  (1u << 3)
#define SPI_CFG         0x004
#define SPI_CFG_AGD     (1u << 0)
#define SPI_IE_RXREADY  (1u << 7)
#define SPI_IE_TXEMPTY  (1u << 8)
#define SPI_IE_COMPLETE (1u << 21)
#define SPI_STATUS      0x008
#define SPI_ST_RXREADY  (1u << 0)
#define SPI_ST_TXEMPTY  (1u << 1)
#define SPI_ST_RXOVF    (1u << 3)
#define SPI_ST_COMPLETE (1u << 22)
#define SPI_ST_FIFOS    ((31u << 4) | (31u << 8))
#define SPI_TXDATA      0x010
#define SPI_RXDATA      0x020
#define SPI_RXCNT       0x034

#define SPI_FIFO        1024    /* DMA fills whole packets (multitouch: 660) */

typedef struct {
    u8  b[SPI_FIFO];
    u32 head, n;
} spi_fifo_t;

static struct {
    u32 regs[0x80 / 4];
    spi_fifo_t tx, rx;
    bool armed;     /* RUN completed at once with nothing queued (below) */
    u32 panel_cmd;
} spi[3] = {
    { .regs = { [SPI_STATUS / 4] = SPI_ST_TXEMPTY | SPI_ST_COMPLETE } },
    { .regs = { [SPI_STATUS / 4] = SPI_ST_TXEMPTY | SPI_ST_COMPLETE } },
    { .regs = { [SPI_STATUS / 4] = SPI_ST_TXEMPTY | SPI_ST_COMPLETE } },
};

static u32 spi_tx_bytes[3];

u32 spi_tx_total(int n) {
    return spi_tx_bytes[n];
}

static bool fifo_push(spi_fifo_t *f, u8 v) {
    if (f->n == SPI_FIFO)
        return false;
    f->b[(f->head + f->n++) % SPI_FIFO] = v;
    return true;
}

static u8 fifo_pop(spi_fifo_t *f) {
    u8 v = f->b[f->head];
    f->head = (f->head + 1) % SPI_FIFO;
    f->n--;
    return v;
}

/* Merlot panel ID protocol (qemu-ios hw/arm/ipod_touch_lcd_panel.c):
 * a command byte, then the next byte clocked in returns its answer. */
static u8 spi_device(int n, u8 tx) {
    if (n == 2)
        return mt_transfer(tx);
    if (n != 1)
        return 0;
    u32 *cmd = &spi[1].panel_cmd;
    if (!*cmd && (tx == 0x95 || tx == 0xDA || tx == 0xDB || tx == 0xDC)) {
        *cmd = tx;
        return 0;
    }
    if (*cmd) {
        u8 r = *cmd == 0x95 ? 0x01 : *cmd == 0xDA ? 0x71 : *cmd == 0xDB ? 0xC2 : 0x00;
        *cmd = 0;
        return r;
    }
    return 0;
}

static u32 spi_word_size(int n) {
    u32 w = (spi[n].regs[SPI_CFG / 4] >> 13) & 3;
    return w == 1 ? 2 : w == 2 ? 4 : 1;
}

static void spi_update_irq(int n) {
    u32 cfg = spi[n].regs[SPI_CFG / 4], mask = 0;
    if (cfg & SPI_IE_RXREADY)  mask |= SPI_ST_RXREADY;
    if (cfg & SPI_IE_TXEMPTY)  mask |= SPI_ST_TXEMPTY;
    if (cfg & SPI_IE_COMPLETE) mask |= SPI_ST_COMPLETE;
    vic_set_irq(VIC_LINE_SPI0 + n, (spi[n].regs[SPI_STATUS / 4] & mask) != 0);
}

static const u32 spi_base[3] = { 0x3C300000u, 0x3CE00000u, 0x3D200000u };

/* DMA pacing: TX room / RX bytes held by an SPI data register. */
u32 periph_dma_avail(u32 pa, bool to_device) {
    for (int n = 0; n < 3; n++) {
        if (to_device && pa == spi_base[n] + SPI_TXDATA)
            return SPI_FIFO - spi[n].tx.n;
        if (!to_device && pa == spi_base[n] + SPI_RXDATA)
            return spi[n].rx.n;
    }
    return 0;
}

static void spi_clock(int n);

/* Run a transfer: let DMA fill TX, clock, let DMA drain RX; repeat while
 * DMA keeps supplying bytes. */
static void spi_run(int n) {
    static bool shown;
    CRUMB_IN(4);
    for (int pass = 0; ; pass++) {
        if (pass >= 256) {              /* the next access or beat goes on */
            hang_cap(&shown, "[SPI] cap bus", (u32)n, spi[n].tx.n, spi[n].rx.n,
                     spi[n].regs[SPI_RXCNT / 4]);
            break;
        }
        pl080_service();
        if (!(spi[n].regs[SPI_CTRL / 4] & SPI_CTRL_RUN))
            break;
        u32 before = spi_tx_bytes[n] + spi[n].regs[SPI_RXCNT / 4];
        spi_clock(n);
        pl080_service();
        u32 after = spi_tx_bytes[n] + spi[n].regs[SPI_RXCNT / 4];
        /* stop when done, when nothing is queued, or when a pass clocked
         * nothing (RX FIFO full: the reader has to drain it first) */
        if (!(spi[n].regs[SPI_CTRL / 4] & SPI_CTRL_RUN) || !spi[n].tx.n || after == before)
            break;
    }
    CRUMB_OUT();
}

/* "S2 c<CTRL> g<CFG> s<STATUS> x<RXCNT> t<TX> r<RX> a<armed>
 *  m<mt cmd now>/<last> b<byte>/<of>" */
void spi2_status_line(char *buf) {
    static const char hx[] = "0123456789ABCDEF";
    extern void mt_debug(u32 *, u32 *, u32 *, u32 *);
    u32 cur, last, ind, size, *r = spi[2].regs;
    mt_debug(&cur, &last, &ind, &size);
    char *p = buf;
#define PUT(tag, v, nd) do { *p++ = ' '; *p++ = tag; \
        for (int i_ = (nd) - 1; i_ >= 0; i_--) *p++ = hx[((v) >> (4 * i_)) & 0xF]; } while (0)
    *p++ = 'S'; *p++ = '2';
    PUT('c', r[SPI_CTRL / 4], 2);
    PUT('g', r[SPI_CFG / 4], 3);
    PUT('s', r[SPI_STATUS / 4], 4);
    PUT('x', r[SPI_RXCNT / 4], 2);
    PUT('t', spi[2].tx.n, 3);
    PUT('r', spi[2].rx.n, 3);
    PUT('a', spi[2].armed, 1);
    PUT('m', cur, 2); *p++ = '/'; p[0] = hx[(last >> 4) & 0xF]; p[1] = hx[last & 0xF]; p += 2;
    PUT('b', ind, 3); *p++ = '/'; for (int i = 2; i >= 0; i--) *p++ = hx[(size >> (4 * i)) & 0xF];
#undef PUT
    *p = 0;
}

/* Heartbeat: keep running buses that still have work.  A driver asleep
 * waiting for its TX DMA's completion makes no register accesses, so a
 * transfer longer than one spi_run's pass budget relies on this. */
void spi_tick(void) {
    for (int n = 0; n < 3; n++) {
        u32 *r = spi[n].regs;
        if (!(r[SPI_CTRL / 4] & SPI_CTRL_RUN) && !spi[n].tx.n)
            continue;
        if (r[SPI_CTRL / 4] & SPI_CTRL_RUN) {
            spi_run(n);
            spi_update_irq(n);
        }
    }
}

static void spi_clock(int n) {
    u32 *r = spi[n].regs;
    if (!(r[SPI_CTRL / 4] & SPI_CTRL_RUN))
        return;
    while (spi[n].tx.n) {
        spi_tx_bytes[n]++;
        u8 rx = spi_device(n, fifo_pop(&spi[n].tx));
        if (!spi[n].tx.n)
            r[SPI_STATUS / 4] |= SPI_ST_TXEMPTY;
        if (r[SPI_RXCNT / 4]) {
            if (!fifo_push(&spi[n].rx, rx)) {
                r[SPI_STATUS / 4] |= SPI_ST_RXOVF;
            } else {
                r[SPI_RXCNT / 4]--;
                r[SPI_STATUS / 4] |= SPI_ST_RXREADY;
            }
        }
    }
    while (spi[n].rx.n < SPI_FIFO && r[SPI_RXCNT / 4] && (r[SPI_CFG / 4] & SPI_CFG_AGD)) {
        fifo_push(&spi[n].rx, spi_device(n, 0xFF));
        r[SPI_RXCNT / 4]--;
        r[SPI_STATUS / 4] |= SPI_ST_RXREADY;
    }
    /* Done only when the TX DMA has nothing more to give: a transfer
     * longer than the FIFO (multitouch HBPP packets, 0xFDD bytes) drains
     * it many times, and clearing RUN at the first empty FIFO left the
     * rest of the DMA stuck behind a full FIFO that never clocked. */
    if (!r[SPI_RXCNT / 4] && !spi[n].tx.n && !pl080_feeds(spi_base[n] + SPI_TXDATA)) {
        r[SPI_STATUS / 4] |= SPI_ST_COMPLETE;
        r[SPI_CTRL / 4] &= ~SPI_CTRL_RUN;
    }
}

static u32 spi_access(int n, u32 off, u32 value, bool is_write) {
    u32 *r = spi[n].regs;
    if (off >= sizeof(spi[n].regs))
        return 0;
    if (!is_write) {
        /* Any read of the controller (status / FIFO / count polling with
         * IRQs masked) lets its DMA channels and the bus make progress. */
        if (off != SPI_RXDATA) {        /* RXDATA is also the DMA's own read */
            spi_run(n);
            pl080_tick();
        }
        u32 v = r[off / 4];
        if (off == SPI_RXDATA) {
            v = 0;
            u32 ws = spi_word_size(n);
            for (u32 i = 0; i < ws && spi[n].rx.n; i++)
                v |= (u32)fifo_pop(&spi[n].rx) << (8 * i);
            if (!spi[n].rx.n)
                spi_run(n);
        } else if (off == SPI_STATUS) {
            v &= ~SPI_ST_FIFOS;
            /* 4-bit levels (the driver reads (STATUS >> 8) & 0xF): our
             * FIFOs are deeper, so saturate instead of wrapping to 0. */
            u32 tl = spi[n].tx.n > 15 ? 15 : spi[n].tx.n;
            u32 rl = spi[n].rx.n > 15 ? 15 : spi[n].rx.n;
            v |= ((tl << 4) | (rl << 8)) & SPI_ST_FIFOS;
        }
        spi_update_irq(n);
        return v;
    }
    bool run = false;
    switch (off) {
    case SPI_CTRL:
        /* AppleS5L8900XSPIController writes CTRL once per transaction,
         * right before asserting chip select (c0494954) */
        if (n == 2)
            mt_cs();
        if (value & SPI_CTRL_TXRST)
            spi[n].tx.n = 0;
        if (value & SPI_CTRL_RXRST)
            spi[n].rx.n = 0;
        r[SPI_CTRL / 4] = value;
        spi[n].armed = false;
        if (value & SPI_CTRL_RUN)
            pl080_service();                /* DMA may supply the TX bytes */
        run = (value & SPI_CTRL_RUN) && spi[n].tx.n;
        if ((value & SPI_CTRL_RUN) && !spi[n].tx.n && !r[SPI_RXCNT / 4]) {
            /* Nothing to clock: complete at once (the old stub's
             * behaviour, which the SPI0 driver's probe relies on).  But
             * the DMA path (AppleS5L8900XSPIController c0494954) writes
             * CTRL=RUN first, then starts the TX DMA, writes RXCNT and
             * finally CFG |= 0x40, and spins until the TX FIFO drains.
             * Remember the RUN so that CFG write can resume it. */
            r[SPI_STATUS / 4] |= SPI_ST_COMPLETE | SPI_ST_TXEMPTY;
            r[SPI_CTRL / 4] &= ~SPI_CTRL_RUN;
            spi[n].armed = true;
        }
        break;
    case SPI_STATUS:
        r[SPI_STATUS / 4] &= ~value;
        run = true;
        break;
    case SPI_TXDATA: {
        u32 ws = spi_word_size(n);
        for (u32 i = 0; i < ws; i++)
            fifo_push(&spi[n].tx, (u8)(value >> (8 * i)));
        if (spi[n].armed) {             /* data for the RUN given earlier */
            spi[n].armed = false;
            r[SPI_CTRL / 4] |= SPI_CTRL_RUN;
            r[SPI_STATUS / 4] &= ~SPI_ST_COMPLETE;
        }
        break;
    }
    case SPI_CFG:
        r[SPI_CFG / 4] = value;
        if (spi[n].armed) {
            pl080_service();                /* TX DMA fills the FIFO */
            if (spi[n].tx.n || r[SPI_RXCNT / 4]) {
                spi[n].armed = false;
                r[SPI_CTRL / 4] |= SPI_CTRL_RUN;
                r[SPI_STATUS / 4] &= ~SPI_ST_COMPLETE;
            }
        }
        run = true;
        break;
    default:
        r[off / 4] = value;
        break;
    }
    if (run)
        spi_run(n);
    spi_update_irq(n);
    return 0;
}

u32 periph_dispatch(u32 addr, u32 value, bool is_write, u32 width)
{
    (void)width;  /* Width not used by individual handlers yet */

    /* CLCD display controller: 0x38900000 - 0x38900FFF */
    if (addr >= 0x38900000 && addr < 0x38901000) {
        if (is_write) {
            clcd_write(addr - 0x38900000, value);
            return 0;
        }
        return clcd_read(addr - 0x38900000);
    }

    /* SHA-1 engine: 0x38000000 - 0x38000FFF */
    if (addr >= 0x38000000 && addr < 0x38001000) {
        if (is_write) {
            sha1_write(addr - 0x38000000, value);
            return 0;
        }
        return sha1_read(addr - 0x38000000);
    }

    /* AES engine: 0x38C00000 - 0x38C00FFF */
    if (addr >= 0x38C00000 && addr < 0x38C01000) {
        if (is_write) {
            aes_write(addr - 0x38C00000, value);
            return 0;
        }
        return aes_read(addr - 0x38C00000);
    }

    /* VIC0: 0x38E00000 - 0x38E00FFF */
    if (addr >= 0x38E00000 && addr < 0x38E01000) {
        if (is_write) {
            vic_write(0, addr - 0x38E00000, value);
            return 0;
        }
        return vic_read(0, addr - 0x38E00000);
    }

    /* VIC1: 0x38E01000 - 0x38E01FFF */
    if (addr >= 0x38E01000 && addr < 0x38E02000) {
        if (is_write) {
            vic_write(1, addr - 0x38E01000, value);
            return 0;
        }
        return vic_read(1, addr - 0x38E01000);
    }

    /* Timer: 0x3E200000 - 0x3E2FFFFF */
    if (addr >= 0x3E200000 && addr < 0x3E300000) {
        if (is_write) {
            timer_write(addr - 0x3E200000, value);
            return 0;
        }
        return timer_read(addr - 0x3E200000);
    }

    /* I2C0 / I2C1 */
    if ((addr & 0xFFF00000) == 0x3C600000 || (addr & 0xFFF00000) == 0x3C900000) {
        int bus = (addr & 0xFFF00000) == 0x3C900000;
        if (is_write) {
            i2c_write(bus, addr & 0xFFF, value);
            return 0;
        }
        return i2c_read(bus, addr & 0xFFF);
    }

    /* UARTs: 0x3CC00000 - 0x3CC13FFF (5 UARTs, 0x4000 each) */
    if (addr >= 0x3CC00000 && addr < 0x3CC14000) {
        int uart = (addr - 0x3CC00000) / 0x4000;
        u32 off = (addr - 0x3CC00000) % 0x4000;
        if (is_write) {
            uart_write(uart, off, value);
            return 0;
        }
        return uart_read(uart, off);
    }

    /* Chip ID: 0x3E500000 - 0x3E5FFFFF */
    if (addr >= 0x3E500000 && addr < 0x3E600000) {
        if (is_write) {
            chipid_write(addr - 0x3E500000, value);
            return 0;
        }
        return chipid_read(addr - 0x3E500000);
    }

    /* GPIO: 0x3E400000 - 0x3E4FFFFF */
    if (addr >= 0x3E400000 && addr < 0x3E500000) {
        if (is_write) {
            gpio_write(addr - 0x3E400000, value);
            return 0;
        }
        return gpio_read(addr - 0x3E400000);
    }

    /* Clock controller 0: 0x38100000 - 0x381FFFFF */
    if (addr >= 0x38100000 && addr < 0x38200000) {
        if (is_write) {
            clock_write(0, addr - 0x38100000, value);
            return 0;
        }
        return clock_read(0, addr - 0x38100000);
    }

    /* SPI0-2 */
    {
        u32 b = addr & 0xFFF00000;
        int n = b == 0x3C300000 ? 0 : b == 0x3CE00000 ? 1 :
                b == 0x3D200000 ? 2 : -1;
        if (n >= 0)
            return spi_access(n, addr & 0xFFF, value, is_write);
    }

    /* Chip ID as the kernel reads it: 0x3D100000 +0 = 0x8900 */
    if ((addr & 0xFFFFF000) == 0x3D100000)
        return (!is_write && (addr & 0xFFF) == 0) ? 0x8900 : 0;

    /* Clock controller 1: 0x3C500000 - 0x3C5FFFFF */
    if (addr >= 0x3C500000 && addr < 0x3C600000) {
        if (is_write) {
            clock_write(1, addr - 0x3C500000, value);
            return 0;
        }
        return clock_read(1, addr - 0x3C500000);
    }

    /* System IC: 0x39A00000 - 0x39AFFFFF */
    if (addr >= 0x39A00000 && addr < 0x39B00000) {
        if (is_write) {
            sysic_write(addr - 0x39A00000, value);
            return 0;
        }
        return sysic_read(addr - 0x39A00000);
    }

    /* NAND: ADM 0x38800000, FMC 0x38A00000, RS-ECC 0x38F00000 */
    if ((addr & 0xFFFFF000) == NAND_ADM_BASE)
        return is_write ? (nand_adm_write(addr & 0xFFF, value), 0)
                        : nand_adm_read(addr & 0xFFF);
    if ((addr & 0xFFFFF000) == NAND_FMC_BASE)
        return is_write ? (nand_fmc_write(addr & 0xFFF, value), 0)
                        : nand_fmc_read(addr & 0xFFF);
    if ((addr & 0xFFFFF000) == NAND_ECC_BASE)
        return is_write ? (nand_ecc_write(addr & 0xFFF, value), 0)
                        : nand_ecc_read(addr & 0xFFF);

    /* PL080 DMA controllers */
    if ((addr & 0xFFFFF000) == PL080_DMAC0_BASE || (addr & 0xFFFFF000) == PL080_DMAC1_BASE) {
        int n = (addr & 0xFFFFF000) == PL080_DMAC1_BASE;
        return is_write ? (pl080_write(n, addr & 0xFFF, value), 0)
                        : pl080_read(n, addr & 0xFFF);
    }

    /* Unknown peripheral -- log (first few) and return 0 */
    static u32 unknown_logged;
    if (unknown_logged >= 64)
        return 0;
    unknown_logged++;
    extern u32 htl_ctx_pc, htl_ctx_va, htl_ctx_mode;
    console_print("HTL: Unknown IO ");
    console_print_hex(addr);
    if (is_write) {
        console_print(" W=");
        console_print_hex(value);
    } else {
        console_print(" R");
    }
    console_print("\n @");
    console_print_hex(htl_ctx_pc);
    console_print(" VA=");
    console_print_hex(htl_ctx_va);
    console_print(" m=");
    console_print_hex(htl_ctx_mode);
    console_print("\n");

    return 0;
}
