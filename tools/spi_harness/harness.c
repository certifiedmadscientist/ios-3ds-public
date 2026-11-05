/*
 * SPI2 + PL080 (dmac1) replay of AppleS5L8900XSPIController's transfer
 * path (c0494954) against the multitouch model, on the host; then the
 * AppleMultitouchZ2SPI frame protocol (0xEB/0xEA two-stage reads, ATN_ACK,
 * long control writes) driven by the GPIO 0x9B interrupt through sysic.c
 * for touch down / drag / up sequences.
 *
 * Per transfer, in the driver's order:
 *   CTRL = RUN (FIFO empty)            -> early-completion shortcut
 *   dmac1 ch: mem -> SPI2 TXDATA, LLI chain of 2 KB blocks (size field is 12 bits), TC on the last
 *   RXCNT = 0, CFG |= 0x40
 *   sleep until the DMA TC interrupt   (only heartbeats run meanwhile)
 *   spin until STATUS TX level == 0
 * The multitouch model sees every byte.  Includes a 40 KB HBPP firmware
 * data packet (0x30), larger than one spi_run's pass budget.
 */
#include <stdio.h>
#include <stdint.h>
#include <stdbool.h>
#include <string.h>
#include <stdlib.h>

typedef uint32_t u32; typedef uint16_t u16; typedef int16_t s16; typedef uint8_t u8;
u32  periph_dispatch(u32 addr, u32 value, bool is_write, u32 width);
void pl080_init(void);
void pl080_write(int n, u32 off, u32 val);
u32  pl080_read(int n, u32 off);
void pl080_tick(void);
void spi_tick(void) __attribute__((weak));
u32  spi_tx_total(int n);

/* host RAM for nhal_phys_* */
#define RAM_PA 0x21000000u
#define RAM_SZ (1u << 20)
static u8 ram[RAM_SZ];
bool nhal_phys_read(u32 pa, void *d, u32 n) {
    if (pa < RAM_PA || pa + n > RAM_PA + RAM_SZ) return false;
    memcpy(d, ram + pa - RAM_PA, n); return true;
}
bool nhal_phys_write(u32 pa, const void *s, u32 n) {
    if (pa < RAM_PA || pa + n > RAM_PA + RAM_SZ) return false;
    memcpy(ram + pa - RAM_PA, s, n); return true;
}
static int irq17;
void nhal_irq(int line, bool level) { if (line == 17) irq17 = level; }
void nhal_puts(const char *s) { printf("  log: %s\n", s); }
void nhal_log(const char *m, u32 v) { printf("  log: %s%08x\n", m, v); }
u32 nhal_ticks(void) { return 0; }
/* everything else the linked objects reference is unused here */
void vic_set_irq(int l, bool v) { (void)l; (void)v; }
u32  sysic_read(u32 offset);
void sysic_write(u32 offset, u32 value);
void input_touch_sample(bool down, u32 rx, u32 ry, u32 now_ms);
void mt_tick(u32 now_ms);
void input_status_line(char *buf);
#include "stubs.inc"

#define SPI2 0x3D200000u
static void w(u32 off, u32 v) { periph_dispatch(SPI2 + off, v, true, 4); }
static u32 r(u32 off) { return periph_dispatch(SPI2 + off, 0, false, 4); }

static int failures;
static int transfer(const u8 *data, u32 len) {
    /* descriptors at RAM_PA, payload at RAM_PA + 0x10000 */
    u32 buf = RAM_PA + 0x10000, desc = RAM_PA;
    memcpy(ram + 0x10000, data, len);
    u32 nblk = (len + 2047) / 2048;
    for (u32 b = 0; b < nblk; b++) {
        u32 n = len - b * 2048 > 2048 ? 2048 : len - b * 2048;
        u32 *d = (u32 *)(ram + (desc - RAM_PA) + 16 * b);
        d[0] = buf + b * 2048; d[1] = SPI2 + 0x10;
        d[2] = b + 1 < nblk ? desc + 16 * (b + 1) : 0;
        d[3] = n | (1u << 26) | (b + 1 == nblk ? 1u << 31 : 0); /* SI, byte, TC on last */
    }
    u32 clocked0 = spi_tx_total(2);
    w(0x00, 0x0D);                                /* CTRL = RUN|TXRST|RXRST */
    u32 *d0 = (u32 *)(ram + (desc - RAM_PA));
    pl080_write(1, 0x030, 1);
    pl080_write(1, 0x100, d0[0]); pl080_write(1, 0x104, d0[1]);
    pl080_write(1, 0x108, d0[2]); pl080_write(1, 0x10C, d0[3]);
    pl080_write(1, 0x110, 1u | (1u << 15) | (1u << 11));   /* mem->periph */
    w(0x34, 0);                                   /* RXCNT */
    w(0x04, r(0x04) | 0x40);                      /* CFG |= 0x40 */
    int beats = 0;
    while (!irq17 && beats < 1000) { pl080_tick(); if (spi_tick) spi_tick(); beats++; }
    if (!irq17) { printf("FAIL: TC never raised (%u bytes)\n", len); return -1; }
    pl080_write(1, 0x008, 0xFF);                  /* filter clears TC */
    int polls = 0;
    while ((r(0x08) & 0xF0) && polls < 100000) polls++;
    if (r(0x08) & 0xF0) { printf("FAIL: TX FIFO never drained\n"); return -1; }
    u32 clocked = spi_tx_total(2) - clocked0;
    if (clocked != len) {
        printf("FAIL: %u of %u bytes clocked\n", clocked, len);
        return -1;
    }
    return beats;
}

/* ── PIO transaction, as c0494954 does it for <= 255 bytes ───────── */
static void pio(const u8 *tx, u32 len, u8 *rx) {
    w(0x08, 0xF);                                 /* STATUS clear */
    w(0x04, 0);                                   /* CFG */
    w(0x00, 0x0D);                                /* CTRL: once per transaction */
    w(0x34, len);                                 /* RXCNT */
    for (u32 i = 0; i < len; i += 8) {
        u32 n = len - i < 8 ? len - i : 8;
        for (u32 k = 0; k < n; k++) w(0x10, tx[i + k]);
        r(0x08);                                  /* status poll clocks */
        for (u32 k = 0; k < n; k++) rx[i + k] = (u8)r(0x20);
    }
}

static u16 sum16(const u8 *p, u32 n) { u32 c = 0; while (n--) c += *p++; return (u16)c; }
static int check(bool ok, const char *what) {
    if (!ok) { printf("FAIL: %s\n", what); failures++; }
    return ok;
}

/* AppleMultitouchZ2SPI readOneFrameOfData (c044ad50): length read
 * (c044b2dc), then the data read (c044b4b8) with its checks.  Returns the
 * finger event (0..7), -1 with no frame, -2 on a protocol error. */
static int frame_read(u8 c, u8 seq, s16 *x) {
    u8 tx[128] = { c, seq, 0 }, rx[128];
    u16 ck = sum16(tx, 14); tx[14] = (u8)ck; tx[15] = ck >> 8;
    pio(tx, 16, rx);
    if (sum16(rx, 14) != (rx[14] | rx[15] << 8)) { printf("  length checksum\n"); return -2; }
    u32 len = rx[1] | rx[2] << 8;
    if (!len) return -1;
    if (len > 0x294) { printf("  length %x\n", len); return -2; }
    u32 n = len + 5;
    memset(tx, 0, sizeof tx); tx[0] = c; tx[1] = seq; tx[2] = 1;
    ck = sum16(tx, 14); tx[n - 2] = (u8)ck; tx[n - 1] = ck >> 8;
    pio(tx, n, rx);
    if (rx[0] != c) { printf("  rx[0]=%02x\n", rx[0]); return -2; }
    if (sum16(rx, 5) & 0xFF) { printf("  header sum %02x\n", sum16(rx, 5) & 0xFF); return -2; }
    u32 dl = rx[2] | rx[3] << 8;
    if (dl != len) { printf("  dataLength %x != %x\n", dl, len); return -2; }
    if (sum16(rx + 5, dl - 2) != (rx[n - 2] | rx[n - 1] << 8)) { printf("  data checksum\n"); return -2; }
    const u8 *h = rx + 5, *f = h + h[2];          /* header, headerLen */
    if (h[0] != 0x44 || h[16] != 1) { printf("  frame type/fingers\n"); return -2; }
    *x = (s16)(f[4] | f[5] << 8);
    return f[1];
}

/* Kernel side of GPIO 0x9B (group 4 bit 27): ack, then one frame read per
 * interrupt (IOInterruptEventSource -> interruptOccurred c044aa90). */

static u8 seq_cmd = 0xEB, seq_no = 1;
static int got[64], ngot, nerr;
static s16 gotx[64];
static void service(void) {
    u32 st = sysic_read(0xA0 + 16), en = sysic_read(0xC0 + 16);
    if (!(st & en & (1u << 27))) return;
    sysic_write(0xA0 + 16, 1u << 27);
    s16 x = 0;
    int e = frame_read(seq_cmd, seq_no, &x);
    if (e == -2) { nerr++; return; }
    if (e < 0) return;
    if (seq_cmd == 0xEB) seq_no = seq_no == 1 ? 2 : 1;
    if (ngot < 64) { gotx[ngot] = x; got[ngot++] = e; }
}

/* A gesture: touch at t=0, drag right until up_ms, then idle; the kernel
 * services its interrupt every svc_ms. */
static void gesture(const char *name, u32 t0, u32 up_ms, u32 svc_ms, const int *want, int nwant) {
    ngot = nerr = 0;
    for (u32 t = 0; t <= up_ms + 600; t += 20) {
        bool down = t <= up_ms;
        input_touch_sample(down, 1000 + t * 4, 2000, t0 + t);
        mt_tick(t0 + t);
        if (t % svc_ms == 0) service();
    }
    for (int i = 0; i < 40; i++) { mt_tick(t0 + up_ms + 700 + i * 50); service(); }
    int ok = ngot == nwant && !nerr;
    for (int i = 0; ok && i < nwant; i++) ok = got[i] == want[i];
    for (int i = 1; ok && i < ngot - 2; i++) ok = gotx[i] >= gotx[i - 1];
    printf("%s: %d frames, events", name, ngot);
    for (int i = 0; i < ngot; i++) printf(" %d", got[i]);
    printf("%s\n", ok ? "" : "  <- unexpected");
    check(ok, name);
}

int main(void) {
    pl080_init();
    static u8 small[16] = { 0xE2 }, fw[10 + 40960];
    /* 34 short commands, then an HBPP data packet with a 40 KB payload */
    for (int i = 0; i < 34 && !getenv("SKIP"); i++) {
        int b = transfer(small, sizeof small);
        if (b < 0) { failures++; break; }
    }
    fw[0] = 0x30; fw[2] = 40960 >> 10; fw[3] = (40960 >> 2) & 0xFF;
    int b = transfer(fw, sizeof fw);
    if (b < 0) failures++;
    else printf("40 KB HBPP packet: TC after %d heartbeats\n", b);
    /* the transfer seen stalled on hardware: 0xFDD bytes, 1 KB FIFO */
    static u8 pkt[0xFDD];
    pkt[0] = 0x30; pkt[2] = 0; pkt[3] = (0xFDD - 15) >> 2;
    b = transfer(pkt, sizeof pkt);
    if (b < 0) failures++;
    else printf("4061-byte packet through 1 KB FIFO: TC after %d heartbeats\n", b);

    /* The 0xBFE8-byte Constructed Firmware (18 E1 + one HBPP packet) split
     * over several SPI transactions; its payload holds command-like bytes
     * (0x1E would turn the next ATN_ACK into 4AD1, 0x30 would swallow
     * traffic).  The packet has to survive the chip-select edges. */
    {
        static u8 blob[0xBFE8];
        u32 len = 0xBFD8;
        blob[0] = 0x18; blob[1] = 0xE1;
        u8 *pk = blob + 2;
        pk[0] = 0x30; pk[1] = 1; pk[2] = len >> 10; pk[3] = (len >> 2) & 0xFF;
        u16 hk = sum16(pk + 2, 6); pk[8] = hk >> 8; pk[9] = (u8)hk;
        for (u32 i = 0; i < len; i++) pk[10 + i] = (u8)(i * 7 + 0x1E);  /* 0x1E, 0x25, ... 0x30 ... */
        u32 piece = 0xFDD, sent = 0;
        for (u32 k = 1; k * piece < sizeof blob - 4; k++)          /* each piece starts */
            blob[k * piece] = k & 1 ? 0x1E : 0x30;                  /* with a command byte */
        int bad = 0;
        while (sent < sizeof blob) {
            u32 n = sizeof blob - sent < piece ? sizeof blob - sent : piece;
            if (transfer(blob + sent, n) < 0) bad = 1;
            sent += n;
        }
        check(!bad, "split firmware transfers");
        u8 a[8] = { 0x1A, 0xA1, 0x18, 0xE1, 0x18, 0xE1, 0x18, 0xE1 }, rr[8];
        pio(a, 8, rr);
        printf("firmware in %u-byte pieces, then ATN_ACK: %02X %02X\n", piece, rr[0], rr[1]);
        check(rr[0] == 0x4B && rr[1] == 0xC1, "ATN_ACK 4BC1 after split firmware");
        u8 iv[16] = { 0xE2 }, ivr[16];
        pio(iv, 16, ivr);
        check(ivr[0] == 0xE2 && ivr[2] == 1 && (ivr[3] | ivr[4] << 8) == 0x294 &&
              sum16(ivr, 14) == (ivr[14] | ivr[15] << 8), "interface version after split firmware");
    }

    /* ATN_ACK after the firmware: 1A A1 18 E1 18 E1 18 E1 in one transaction */
    u8 ack[8] = { 0x1A, 0xA1, 0x18, 0xE1, 0x18, 0xE1, 0x18, 0xE1 }, rx[64];
    pio(ack, 8, rx);
    check(rx[0] == 0x4B && rx[1] == 0xC1, "HBPP ATN_ACK 4BC1");
    printf("ATN_ACK: %02X %02X\n", rx[0], rx[1]);

    /* A long control write (0xE5) whose payload holds a 0x30 HBPP-looking
     * header, then GET_CMD_STATUS: the old model parsed it as commands */
    u8 lw1[16] = { 0xE5, 0xA2, 0, 8, 0 }, lw2[13] = { 0xE5, 0xA2, 1, 0x30, 0x01, 0x04, 0x0F, 0x18, 0x1E, 0xEA };
    u16 ck = sum16(lw1, 14); lw1[14] = (u8)ck; lw1[15] = ck >> 8;
    pio(lw1, 16, rx); pio(lw2, 13, rx);
    u8 cs[16] = { 0xE1 }; ck = sum16(cs, 14); cs[14] = (u8)ck; cs[15] = ck >> 8;
    pio(cs, 16, rx);
    check(rx[0] == 0xE1 && sum16(rx, 14) == (rx[14] | rx[15] << 8) && rx[8] == 0, "CMD_STATUS after long write");
    printf("long write + CMD_STATUS: rx[0]=%02X status=%02X\n", rx[0], rx[8]);

    /* Long control read (0xE7) of the 8-byte sensor dimensions report */
    u8 lr1[16] = { 0xE7, 0xD9, 0, 8, 0 }, lr2[13] = { 0xE7, 0xD9, 1 };
    ck = sum16(lr1, 14); lr1[14] = (u8)ck; lr1[15] = ck >> 8;
    pio(lr1, 16, rx); pio(lr2, 13, rx);
    check(rx[0] == 0xE7 && sum16(rx, 11) == (rx[11] | rx[12] << 8) &&
          (rx[3] | rx[4] << 8) == 5000 && (rx[7] | rx[8] << 8) == 7500, "long control read D9");
    printf("long read D9: width %u height %u\n", rx[3] | rx[4] << 8, rx[7] | rx[8] << 8);

    /* Re-bootload after the chip was powered down (Home / wake): the first
     * boot's calibration register writes (0x1E -> ack 4AD1) must not leave
     * the next firmware or calibration packet's ATN_ACK at 4AD1; the
     * driver needs 4BC1 there or kills the device ("No firmware ..."). */
    {
        u8 wr[16] = { 0x1E, 0x33, 0, 0, 0x10, 0, 0, 0x84, 0, 0, 0, 0xFF }, a[8] = { 0x1A, 0xA1, 0x18, 0xE1, 0x18, 0xE1, 0x18, 0xE1 }, rr[16];
        pio(wr, 16, rr); pio(a, 8, rr);
        check(rr[0] == 0x4A && rr[1] == 0xD1, "ATN_ACK 4AD1 after a register write");
        int ok = 1;
        for (int boot = 0; boot < 3; boot++) {
            u8 chk[16] = { 0x1A, 0xA1 }; for (int i = 2; i < 16; i += 2) { chk[i] = 0x18; chk[i + 1] = 0xE1; }
            pio(chk, 16, rr);                       /* checking if in HBPP */
            u32 w0 = rr[0] << 8 | rr[1];
            ok &= w0 == 0x4BC1 || w0 == 0x4AD1;
            static u8 fw[2 + 10 + 0x400 + 4];       /* 18 E1 + HBPP packet */
            u32 len = 0x400;
            fw[0] = 0x18; fw[1] = 0xE1; fw[2] = 0x30; fw[3] = 1; fw[4] = len >> 10; fw[5] = (len >> 2) & 0xFF;
            transfer(fw, sizeof fw);
            pio(a, 8, rr);
            printf("re-bootload %d: firmware ATN_ACK %02X %02X", boot, rr[0], rr[1]);
            ok &= rr[0] == 0x4B && rr[1] == 0xC1;
            pio(wr, 16, rr); pio(a, 8, rr);         /* calibration register write */
            printf(", register write ATN_ACK %02X %02X\n", rr[0], rr[1]);
            ok &= rr[0] == 0x4A && rr[1] == 0xD1;
        }
        check(ok, "firmware re-upload after register writes acks 4BC1");
    }

    /* Spurious interrupt read with nothing queued: length 0 */
    s16 x;
    check(frame_read(0xEB, 1, &x) == -1, "empty length read");

    sysic_write(0xC0 + 16, 1u << 27);             /* enable GPIO 0x9B */
    /* bcdVersion 51 > 0x23: 0xEB reads with an alternating sequence byte */
    static const int tap[] = { 3, 4, 4, 4, 4, 7, 0 };
    gesture("EB down/drag/up, kernel every 20 ms", 1000, 240, 20, tap, 7);
    static const int drag[] = { 3, 4, 4, 4, 4, 4, 4, 4, 4, 4, 4, 4, 4, 4, 4, 4, 4, 7, 0 };  /* 16 moves at 20 Hz */
    gesture("EB 1 s drag, kernel every 20 ms", 5000, 1000, 20, drag, 19);
    gesture("EB drag, kernel slow (every 200 ms)", 9000, 240, 200, tap, 7);
    seq_cmd = 0xEA; seq_no = 1;
    gesture("EA down/drag/up", 13000, 240, 20, tap, 7);
    seq_cmd = 0xEB;
    static const int quick[] = { 3, 7, 0 };
    gesture("EB quick tap", 17000, 20, 20, quick, 3);

    char line[80];
    input_status_line(line);
    printf("TS row (%zu cols): %s\n", strlen(line), line);
    check(strlen(line) <= 50, "TS row fits 50 columns");

    printf(failures ? "RESULT: %d failure(s)\n" : "RESULT: all checks passed\n", failures);
    return failures != 0;
}
