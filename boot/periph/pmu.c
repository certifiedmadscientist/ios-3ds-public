/*
 * PCF50635 PMU, after qemu-ios ipod_touch_pcf50633_pmu.c: the first byte
 * written after a START is the register pointer, every byte read returns
 * that register and advances the pointer.  Data bytes written after the
 * pointer are dropped, except the ADC start command (ADCC1).
 *
 * The RTC runs from a fixed start date plus elapsed timer ticks; the 3DS
 * RTC isn't read, so the date is plausible rather than correct.
 *
 * Power: ApplePCF50635PMUPowerSource (com.apple.driver.ApplePCF50635PMU)
 * derives the whole battery state from two things read here:
 *
 *  - MBCS1 (0x4B, read with MBCS2/3): bit 0 USB present -> USB power; it
 *    then tells a USB host from an Apple charger by D+/D- on ADC channel 2
 *    (two reads; ratio 1000*dP/dM in 901..1120 with dP < 368 -> the 500 mA
 *    charger, type 4).  Bit 2 (adapter) alone is FireWire power, which
 *    never charges.  Types 3-5 publish ExternalConnected and
 *    ExternalChargeCapable; IsCharging then follows the charger's status
 *    GPIO (0x12 pin 0), which reads 0 here, so the battery shows charged.
 *
 *  - ADC channel 0, the battery voltage: ADCC1 = ch << 4 | 0x0D starts it;
 *    ADCS3 bit 7 must be set (else the read fails and capacity sticks at
 *    75), result = ADCS1 << 2 | ADCS3[1:0], mV = code * 6000 / 1023.  The
 *    driver adds a load offset (0..+100 mV) and maps mV to percent with
 *    the kext's "0001-default" battery-data curve: 4200 mV and up is 100.
 *
 * Only those two channels convert; the rest keep failing as before, so no
 * other ADC user (accessory ID, ...) sees invented data.
 *
 * Interrupts (PCF50633 layout, as ApplePCF50635PMU uses it): INT1..INT5 at
 * 0x02..0x06 are read-to-clear (the driver reads all five at 0x02,
 * C047F8AC), masks INT1M..INT5M at 0x07..0x0B (1 = masked); a pending
 * unmasked bit holds the PMU's interrupt line, GPIO interrupt 0x55.  The
 * iPod's Hold and Home buttons also reach the PMU's EXTON1 input: that is
 * PMU interrupt 10 (INT2 bit 2, rising), the "button-wake" device's wake
 * source, decoded at wake as "exton1(buttons)" (C0481594).
 */

#include "pmu.h"
#include "timer.h"
#include "sysic.h"

#define PMU_MBCS1   0x4B
#define PMU_ADCC1   0x54        /* ch << 4 | res/avg | start (bit 0) */
#define PMU_ADCS1   0x55        /* result bits 9:2 */
#define PMU_ADCS3   0x57        /* bit 7 ready, bits 1:0 result bits 1:0 */
#define PMU_RTCSC   0x59
#define PMU_RTCMN   0x5A
#define PMU_RTCHR   0x5B
#define PMU_RTCWD   0x5C
#define PMU_RTCDT   0x5D
#define PMU_RTCMT   0x5E
#define PMU_RTCYR   0x5F

/* RTC start: Wednesday 2026-11-04 12:00:00. */
#define RTC_YEAR    2026
#define RTC_MONTH   11
#define RTC_DAY     4
#define RTC_WDAY    3
#define RTC_SECS    (12u * 3600u)

#define TIMER_HZ    24000000u   /* timer_get_ticks rate with divider 1 */

/* USB present + USB OK: external power from an Apple USB charger. */
#define MBCS1_USB   0x03

/* 10-bit ADC codes.  Battery: 718 -> 4211 mV, a full cell, 100% with no
 * load offset.  D+/D-: equal (ratio 1000) and below 368, so the driver
 * picks the 500 mA Apple charger, not a USB host. */
#define ADC_CH_VBAT     0
#define ADC_CH_USBID    2
#define ADC_VBAT_CODE   718
#define ADC_USBID_CODE  256

#define PMU_INT1    0x02        /* .. INT5 0x06, read-to-clear */
#define PMU_INT1M   0x07        /* .. INT5M 0x0B, 1 = masked */
#define PMU_NINT    5
#define INT2_EXTON1R    (1u << 2)
#define INT2_EXTON1F    (1u << 3)
#define GPIO_IRQ_PMU    0x55

static u8   int_pending[PMU_NINT];
static u8   int_mask[PMU_NINT];          /* reset value 0: all enabled */
u32 pmu_irq_raises, pmu_int_reads;

static void pmu_update_irq(void)
{
    for (u32 i = 0; i < PMU_NINT; i++)
        if (int_pending[i] & ~int_mask[i]) {
            pmu_irq_raises++;
            sysic_gpio_raise(GPIO_IRQ_PMU);
            return;
        }
}

/* Hold / Home pressed or released: EXTON1 rising / falling. */
void pmu_button_event(bool pressed)
{
    int_pending[1] |= pressed ? INT2_EXTON1R : INT2_EXTON1F;
    pmu_update_irq();
}

/* For the status row: INT2 pending and mask. */
u32 pmu_int2_state(void)
{
    return (u32)int_pending[1] << 8 | int_mask[1];
}

static u8   cmd;
static bool cmd_next;           /* next byte written is the pointer */
static bool adc_valid;          /* last conversion was a modelled channel */
static u16  adc_code;

void pmu_i2c_start(void)
{
    cmd_next = true;
}

static void adc_start(u8 ctl)
{
    if (!(ctl & 1))
        return;
    adc_valid = true;
    switch (ctl >> 4) {
    case ADC_CH_VBAT:   adc_code = ADC_VBAT_CODE;  break;
    case ADC_CH_USBID:  adc_code = ADC_USBID_CODE; break;
    default:            adc_valid = false;         break;
    }
}

static void reg_write(u8 reg, u8 val)
{
    if (reg == PMU_ADCC1)
        adc_start(val);
    if (reg >= PMU_INT1M && reg < PMU_INT1M + PMU_NINT) {
        int_mask[reg - PMU_INT1M] = val;
        pmu_update_irq();
    }
}

static u8 to_bcd(u32 v)
{
    return (u8)(((v / 10) << 4) | (v % 10));
}

static bool leap(u32 y)
{
    return (y % 4 == 0 && y % 100 != 0) || y % 400 == 0;
}

static u32 month_days(u32 y, u32 m)
{
    static const u8 d[12] = { 31, 28, 31, 30, 31, 30, 31, 31, 30, 31, 30, 31 };
    return (m == 2 && leap(y)) ? 29 : d[m - 1];
}

static u8 rtc_field(u8 reg)
{
    u32 secs = RTC_SECS + (u32)(timer_get_ticks() / TIMER_HZ);
    u32 days = secs / 86400, tod = secs % 86400;
    u32 y = RTC_YEAR, m = RTC_MONTH, d = RTC_DAY;
    u32 wday = (RTC_WDAY + days) % 7;

    while (days--) {
        if (++d > month_days(y, m)) {
            d = 1;
            if (++m > 12) {
                m = 1;
                y++;
            }
        }
    }

    switch (reg) {
    case PMU_RTCSC: return to_bcd(tod % 60);
    case PMU_RTCMN: return to_bcd(tod / 60 % 60);
    case PMU_RTCHR: return to_bcd(tod / 3600);
    case PMU_RTCWD: return (u8)wday;
    case PMU_RTCDT: return to_bcd(d);
    case PMU_RTCMT: return to_bcd(m);
    case PMU_RTCYR: return to_bcd(y - 2000);
    default:        return 0;
    }
}

void pmu_i2c_send(u8 byte)
{
    if (cmd_next) {
        cmd = byte;
        cmd_next = false;
    } else {
        reg_write(cmd++, byte);
    }
}

u32 pmu_reads, pmu_last_reg;

u8 pmu_i2c_recv(void)
{
    u8 reg = cmd++;
    pmu_reads++;
    pmu_last_reg = reg;

    if (reg >= PMU_INT1 && reg < PMU_INT1 + PMU_NINT) {
        u8 v = int_pending[reg - PMU_INT1];
        int_pending[reg - PMU_INT1] = 0;
        pmu_int_reads++;
        return v;
    }
    if (reg >= PMU_INT1M && reg < PMU_INT1M + PMU_NINT)
        return int_mask[reg - PMU_INT1M];

    switch (reg) {
    case PMU_MBCS1:
        return MBCS1_USB;
    case PMU_ADCS1:
        return adc_valid ? (u8)(adc_code >> 2) : 0;
    case PMU_ADCS3:
        return adc_valid ? (u8)(0x80 | (adc_code & 3)) : 0;
    case PMU_RTCSC ... PMU_RTCYR:
        return rtc_field(reg);
    case 0x67:          /* enable the debug UARTs */
        return 1;
    case 0x69:          /* boot error / panic count */
    default:
        return 0;
    }
}
