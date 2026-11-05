/*
 * Host replay of ApplePCF50635PMUPowerSource's battery path against the
 * PMU model (periph/i2c.c + periph/pmu.c), driving bus 1 the way a Samsung
 * IIC master does.  Addresses are the kext's in the prelinked KERNEL.BIN
 * (com.apple.driver.ApplePCF50635PMU, __text at 0xC047F000):
 *
 *   C047FA18  readADC (gated): 0x52 <- 0; 0x53,0x54 <- 0, ch<<4|0x0D;
 *             ADCS3 (0x57) bit 7 clear -> error; 10-bit = 0x55<<2 | 0x57&3
 *   C04846A8  checkPowerSource: MBCS1..3 from 0x4B; bit 2 -> type 2;
 *             bit 0 -> USB, C04836D8 D+/D- on ADC ch 2 picks type 3/4/5
 *   C0483BC0  capacity: mV = code*6000/1023 (+ load offset), curve lookup
 *             in 16.16, rounded, averaged over the last 4 samples
 *   C04841E4  ext / chargeCap / amperage / IsCharging from the type
 *   C0484428  every other poll IsCharging = charger status GPIO (reads 0)
 *   C0483340  publish: clamp to 95 while charging, FullyCharged
 */
#include <stdio.h>
#include <stdint.h>
#include <stdbool.h>

typedef uint32_t u32; typedef uint16_t u16; typedef uint8_t u8;
typedef uint64_t u64; typedef int64_t s64;

void i2c_init(void);
u32  i2c_read(int bus, u32 offset);
void i2c_write(int bus, u32 offset, u32 value);

void vic_set_irq(int l, bool v) { (void)l; (void)v; }
u32  vic_line_count(int l) { (void)l; return 0; }
void sysic_gpio_raise(u32 irq) { (void)irq; }
u64  timer_get_ticks(void) { return 0; }

#include "curve.inc"    /* "0001-default" battery-data, from KERNEL.BIN */

#define CON 0x00
#define STAT 0x04
#define DS 0x0C
#define PMU 0x73

static void xfer_start(u8 addr_rw, u32 mode)
{
    i2c_write(1, DS, addr_rw);
    i2c_write(1, STAT, mode | 0x30);       /* START | TXRX */
}

static void xfer_stop(u32 mode)
{
    i2c_write(1, STAT, mode | 0x10);       /* TXRX, no START: stop */
}

static void wr(u8 reg, const u8 *d, int n)
{
    xfer_start(PMU << 1, 0xC0);            /* master transmit */
    i2c_write(1, DS, reg);
    for (int i = 0; i < n; i++)
        i2c_write(1, DS, d[i]);
    xfer_stop(0xC0);
}

static void rd(u8 reg, u8 *d, int n)
{
    xfer_start(PMU << 1, 0xC0);
    i2c_write(1, DS, reg);
    xfer_stop(0xC0);
    xfer_start(PMU << 1 | 1, 0x80);        /* master receive */
    for (int i = 0; i < n; i++)
        d[i] = (u8)i2c_read(1, DS);
    xfer_stop(0x80);
}

/* C047FA18 for flags = 0: returns false on the ADCS3 "not ready" error. */
static bool read_adc(int ch, u32 *out)
{
    u8 z = 0, c[2] = { 0, (u8)(ch << 4 | 0x0D) }, s3, s1;
    wr(0x52, &z, 1);
    wr(0x53, c, 2);
    rd(0x57, &s3, 1);
    if (!(s3 & 0x80))
        return false;
    rd(0x55, &s1, 1);
    *out = (u32)s1 << 2 | (s3 & 3);
    return true;
}

/* driver state (ApplePCF50635PMUPowerSource fields) */
static int  type = 1, usbtype = 1;        /* +0xF0, +0xF1 */
static u32  usb_ma = 100;                 /* +0x120 */
static bool ext, chg_cap, charging;       /* +0xF9, +0xFA, +0xFC */
static u32  amps;                         /* +0x134 */
static int  cap = -1;                     /* +0x124 */
static u32  ring[4], ring_sum, ring_n, ring_i;

static int fails;
#define CHECK(c, ...) do { if (!(c)) { fails++; printf("FAIL: "); printf(__VA_ARGS__); printf("\n"); } } while (0)

static u32 code_to_mv(u32 code) { return code * 6000 / 1023; }

/* C0483BC0 with voltage 0 (read the ADC), current argument cur. */
static void update_capacity(u32 cur)
{
    u32 code;
    if (!read_adc(0, &code)) {
        cap = 75;
        return;
    }
    u32 mv = code_to_mv(code), r4 = cur;
    if (r4) {
        switch (type) {
        case 2: r4 = 0; break;
        case 3: r4 -= usb_ma < 500 ? 60 : 100; break;
        case 4: case 5: r4 -= 100; break;
        }
    }
    if (r4 > 18) {
        u8 ledena;
        rd(0x29, &ledena, 1);
        if (!(ledena & 1))
            r4 -= 18;
    }
    mv += r4;

    u64 x = (u64)mv << 16, pc = 0, pv = 0, y = 0;
    for (int i = 0; ; i += 2) {
        u64 c = curve[i], v = curve[i + 1];
        if (v > x) { pc = c; pv = v; continue; }
        y = c + (u64)((s64)((x - v) * (pc - c)) / (s64)(pv - v));
        break;
    }
    u32 sample = (u32)((y + 0x8000) >> 16);
    ring_sum = ring_sum - ring[ring_i & 3] + sample;
    ring[ring_i++ & 3] = sample;
    if (ring_n < 4)
        ring_n++;
    cap = (int)(ring_sum / ring_n);
    printf("  capacity: ADC %u -> %u mV, offset %d -> %u mV, sample %u%%, avg %d%%\n",
           code, code_to_mv(code), (int)r4, mv, sample, cap);
}

/* C04841E4 */
static void set_supply(void)
{
    ext = type != 1;
    chg_cap = type == 2 ? usb_ma >= 500 : type >= 3;
    amps = type == 1 || type == 2 ? 0 : type == 3 ? usb_ma : type == 4 ? 500 : 1000;
    charging = ext && chg_cap;
}

/* C0483340: what IOPMPowerSource ends up publishing. */
static void publish(int *cur, bool *full)
{
    int c = cap;
    if (chg_cap && (unsigned)c > 95)
        c = charging ? 95 : 100;
    *cur = c;
    *full = chg_cap && (unsigned)c > 97 && !charging;
}

int main(void)
{
    i2c_init();

    /* RTC and debug flag must still read through the pointer logic. */
    u8 rtc[7], dbg;
    rd(0x59, rtc, 7);
    rd(0x67, &dbg, 1);
    printf("RTC 20%02x-%02x-%02x %02x:%02x:%02x wday %u, reg 0x67 = %u\n",
           rtc[6], rtc[5], rtc[4], rtc[2], rtc[1], rtc[0], rtc[3], dbg);
    CHECK(rtc[6] == 0x26 && rtc[5] == 0x11 && rtc[4] == 0x04 && rtc[2] == 0x12,
          "RTC reads");
    CHECK(dbg == 1, "debug UART flag");

    /* Channels the model doesn't convert still fail, as before. */
    u32 code;
    CHECK(!read_adc(7, &code), "ADC ch 7 should not convert");

    /* C04846A8 */
    u8 mbcs[3];
    rd(0x4B, mbcs, 3);
    printf("mbcs1-3 %02x %02x %02x\n", mbcs[0], mbcs[1], mbcs[2]);
    if (mbcs[0] & 4)
        type = 2;
    if (mbcs[0] & 1) {
        u32 dp, dm;
        usbtype = 3;
        if (read_adc(2, &dp) && read_adc(2, &dm) && dp > 99 && dm > 99) {
            u32 ratio = dp * 1000 / dm;
            printf("adc dP %u, dM %u, ratio %u\n", dp, dm, ratio);
            if (ratio - 1291 <= 214)
                usbtype = 5;
            else if (ratio - 901 <= 219 && dp < 368)
                usbtype = 4;
        }
        if (type == 1)
            type = usbtype;
    } else {
        usb_ma = 100;
        usbtype = 1;
    }
    printf("_powerSupplyType %d, _usbConnectType %d\n", type, usbtype);
    CHECK(type == 4, "expected the 500 mA Apple charger (type 4)");

    int pub; bool full;
    set_supply();
    publish(&pub, &full);
    printf("first publish (cap still -1): cap %d ext %d chrgCap %d chrg %d (%u mA) full %d\n",
           pub, ext, chg_cap, charging, amps, full);

    update_capacity(0);         /* pmu-measured: current argument 0 */
    charging = false;           /* C0484428: status GPIO reads 0 */
    publish(&pub, &full);
    printf("after check:  cap %d ext %d chrgCap %d chrg %d (%u mA) full %d\n",
           pub, ext, chg_cap, charging, amps, full);
    CHECK(pub == 100 && ext && chg_cap && !charging && full,
          "expected 100%%, external power, charged");

    /* Later polls (current 100), and the same cell on battery power. */
    update_capacity(100);
    CHECK(cap == 100, "poll on charger");
    type = 1;
    update_capacity(100);
    CHECK(cap == 100, "poll on battery");

    puts(fails ? "PMU harness: FAILED" : "PMU harness: OK");
    return fails != 0;
}
