/*
 * S5L8900 timer emulation, PA 0x3E200000 (device tree "timer", reg 0x06200000).
 *
 * Register layout as used by the iPhone OS 1 kernel's timer driver
 * (VA 0xC0160110) and its rtclock hooks (VA 0xC0061650-0xC00616A4):
 *
 *   +0x80 / +0x84  64-bit free-running counter, HIGH / LOW word
 *   +0x88          counter config: ((div-1) << 4) | flags
 *   +0xA0          deadline timer config
 *   +0xA4          deadline timer command (3 = start)
 *   +0xA8          deadline period (counter units)
 *   +0xAC          data1 (written 0xFFFFFFFF)
 *   +0xB4          deadline timer current count (counts down from +0xA8)
 *   +0xF4          interrupt enable mask
 *   +0xF8          interrupt status, write-1-to-clear (FIQ entry writes 0x30000)
 *
 * The input clock is 24 MHz, divided by n = fixed-frequency /
 * timebase-frequency (pe_identify_machine reads both from the device-tree
 * node with device_type "cpu"); the driver writes ((n-1) << 4) | flags to
 * +0x88.  The counter is accumulated from the MPCore watchdog run as a
 * free-running timer: unlike CCNT it keeps counting through WFI.  Its first
 * value continues the CCNT fallback _ml_get_timebase uses before the timer
 * driver registers, so time never goes backwards.
 *
 * The MPCore private timer (GIC ID 29) runs a fixed 100 Hz heartbeat
 * independent of the kernel's deadlines, so our IRQ path (display mirror,
 * status rows) keeps running however long the kernel idles; the deadline
 * is emulated against the 64-bit counter and delivered on the first beat
 * at or after it, as the kernel's FIQ (VIC0 line 7).
 */

#include "../iowin.h"
#include "timer.h"
#include "vic.h"
#include "../console.h"

#define CPU_HZ          268111856u      /* O3DS ARM11 core clock (CCNT rate) */
#define PERIPH_HZ       (CPU_HZ / 2)    /* MPCore private timer clock */
#define S5L_TIMER_HZ    24000000u

#define PRIV_TIMER_LOAD IOREG32(0x17E00600)
#define PRIV_TIMER_CNT  IOREG32(0x17E00604)
#define PRIV_TIMER_CTRL IOREG32(0x17E00608)
#define PRIV_TIMER_ISR  IOREG32(0x17E0060C)

#define WDT_LOAD        IOREG32(0x17E00620)
#define WDT_CNT         IOREG32(0x17E00624)
#define WDT_CTRL        IOREG32(0x17E00628)
#define WDT_ISR         IOREG32(0x17E0062C)
#define WDT_DISABLE     IOREG32(0x17E00634)

#define GICC_CTRL       IOREG32(0x17E00100)
#define GICC_PMR        IOREG32(0x17E00104)
#define GICD_CTRL       IOREG32(0x17E01000)
#define GICD_SET_EN(n)  IOREG32(0x17E01100 + 4 * (n))
#define GICD_CLR_EN(n)  IOREG32(0x17E01180 + 4 * (n))
#define GICD_PEND(n)    IOREG32(0x17E01200 + 4 * (n))
#define GICD_CLR_PEND(n) IOREG32(0x17E01280 + 4 * (n))
#define GICD_PRIO(n)    IOREG32(0x17E01400 + 4 * (n))

#define TIMER_IRQ_ID    29
#define VIC_TIMER_LINE  7

static u32 cnt_hz = S5L_TIMER_HZ;
static bool cnt_started;
static u32 wdt_last;
static u64 cnt_value;
static u64 cnt_frac;            /* remainder of delta*cnt_hz, in 1/PERIPH_HZ units */

static u32 reg_cfg88, reg_cfg, reg_cmd, reg_period, reg_data1, reg_irq_en;
static u32 irq_status;
static u64 dl_start;            /* counter value when the deadline was started */
static u64 dl_next;             /* counter value of the next expiry */
static bool dl_running;

#define HEARTBEAT_HZ    100

extern u32 kern_irq_taken;      /* htl.c: private-timer IRQs taken */

static inline u32 ccnt_read(void)
{
    u32 v;
    __asm__ volatile("mrc p15, 0, %0, c15, c12, 1" : "=r"(v));
    return v;
}

/* Advance the 64-bit counter from the watchdog (down-counting from
 * 0xFFFFFFFF at PERIPH_HZ, wraps every ~32 s; the deadline tick keeps
 * reads far more frequent than that). */
static u64 counter_now(void)
{
    u32 now = WDT_CNT;
    if (!cnt_started) {
        cnt_started = true;
        wdt_last = now;
        cnt_value = ccnt_read();
        return cnt_value;
    }
    u64 acc = (u64)(wdt_last - now) * cnt_hz + cnt_frac;
    wdt_last = now;
    cnt_value += acc / PERIPH_HZ;
    cnt_frac = acc % PERIPH_HZ;
    return cnt_value;
}

/* Once: report if the private timer expired but no IRQ was ever taken. */
static void check_irq_delivery(void)
{
    static bool reported;
    if (reported || kern_irq_taken || !(PRIV_TIMER_ISR & 1))
        return;
    reported = true;
    u32 spsr;
    __asm__ volatile("mrs %0, spsr" : "=r"(spsr));
    console_print("[TMR] expired, no IRQ: CPSR=");
    console_print_hex(spsr);
    console_print(" pend=");
    console_print_hex(GICD_PEND(0));
    console_print("\n en=");
    console_print_hex(GICD_SET_EN(0));
    console_print(" D=");
    console_print_hex(GICD_CTRL);
    console_print(" C=");
    console_print_hex(GICC_CTRL);
    console_print(" ctl=");
    console_print_hex(PRIV_TIMER_CTRL);
    console_print("\n");
}

void timer_init(void)
{
    PRIV_TIMER_CTRL = 0;
    PRIV_TIMER_ISR = 1;

    /* Watchdog as a free-running timer (leave watchdog mode if set). */
    WDT_DISABLE = 0x12345678;
    WDT_DISABLE = 0x87654321;
    WDT_CTRL = 0;
    WDT_ISR = 1;
    WDT_LOAD = 0xFFFFFFFF;
    WDT_CTRL = 0x3;             /* enable | auto-reload, no IRQ, timer mode */

    /* MPCore GIC: only the private timer may interrupt. */
    for (int i = 0; i < 4; i++) {
        GICD_CLR_EN(i) = 0xFFFFFFFF;
        GICD_CLR_PEND(i) = 0xFFFFFFFF;
    }
    GICD_PRIO(TIMER_IRQ_ID / 4) &= ~(0xFFu << (8 * (TIMER_IRQ_ID % 4)));
    /* SGI 1: self-kick used by the IRQ bridge to deliver emulated VIC
     * interrupts as soon as the kernel unmasks IRQs (vic_kick, htl.c). */
    /* Lower priority than the heartbeat: the GIC breaks priority ties by
     * lowest ID, so a level VIC line re-kicking SGI 1 on every handler
     * pass starved timer ID 29 (heartbeat, input, display all stopped). */
    GICD_PRIO(KICK_SGI_ID / 4) = (GICD_PRIO(KICK_SGI_ID / 4) &
                                  ~(0xFFu << (8 * (KICK_SGI_ID % 4)))) |
                                 (0x80u << (8 * (KICK_SGI_ID % 4)));
    GICD_SET_EN(0) = (1u << TIMER_IRQ_ID) | (1u << KICK_SGI_ID);
    GICD_CTRL = 1;
    GICC_PMR = 0xF0;
    GICC_CTRL = 1;

    console_print("[TMR] GIC D=");
    console_print_hex(GICD_CTRL);
    console_print(" C=");
    console_print_hex(GICC_CTRL);
    console_print(" PMR=");
    console_print_hex(GICC_PMR);
    console_print(" en0=");
    console_print_hex(GICD_SET_EN(0));
    console_print("\n");

    irq_status = 0;
    dl_running = false;

    PRIV_TIMER_LOAD = PERIPH_HZ / HEARTBEAT_HZ;
    PRIV_TIMER_CTRL = 0x7;      /* enable | auto-reload | IRQ enable */
}

u32 timer_read(u32 offset)
{
    switch (offset) {
    case 0x80: check_irq_delivery(); return (u32)(counter_now() >> 32);
    case 0x84: return (u32)counter_now();
    case 0x88: return reg_cfg88;
    case 0xA0: return reg_cfg;
    case 0xA4: return reg_cmd;
    case 0xA8: return reg_period;
    case 0xAC: return reg_data1;
    case 0xB4: {
        if (!dl_running || reg_period == 0)
            return 0;
        u64 el = counter_now() - dl_start;
        return reg_period - (u32)(el % reg_period);
    }
    case 0xF4: return reg_irq_en;
    case 0xF8: return irq_status;
    default:   return 0;
    }
}

void timer_write(u32 offset, u32 value)
{
    switch (offset) {
    case 0x88: {
        counter_now();          /* settle the counter at the old rate */
        reg_cfg88 = value;
        u32 div = ((value >> 4) & 0x7FF) + 1;
        cnt_hz = S5L_TIMER_HZ / div;
        console_print("[TMR] +88=");
        console_print_hex(value);
        console_print(" div=");
        console_print_hex(div);
        console_print("\n");
        break;
    }
    case 0xA0: reg_cfg = value; break;
    case 0xA8: reg_period = value; break;
    case 0xAC: reg_data1 = value; break;
    case 0xA4:
        reg_cmd = value;
        if (value & 1) {
            dl_start = counter_now();
            dl_next = dl_start + (reg_period ? reg_period : 1);
            dl_running = true;
        } else {
            dl_running = false;
        }
        break;
    case 0xF4: reg_irq_en = value; break;
    case 0xF8:
        irq_status &= ~value;
        if (!irq_status)
            vic_set_irq(VIC_TIMER_LINE, false);
        break;
    default:
        break;
    }
}

/* Heartbeat IRQ: returns true if the kernel's deadline has expired and
 * its timer FIQ is now raised.  Periodic like the hardware timer; if beats
 * were missed, the next expiry is rebased instead of firing in a burst. */
bool timer_heartbeat(void)
{
    PRIV_TIMER_ISR = 1;
    if (!dl_running)
        return false;
    u64 now = counter_now();
    if (now < dl_next)
        return false;
    u64 period = reg_period ? reg_period : 1;
    dl_next += period;
    if (dl_next <= now)
        dl_next = now + period;
    irq_status |= 0x30000;      /* bits the kernel's FIQ entry clears */
    vic_set_irq(VIC_TIMER_LINE, true);
    return true;
}

u64 timer_get_ticks(void)
{
    return counter_now();
}
