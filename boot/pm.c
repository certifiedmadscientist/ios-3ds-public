/*
 * pm.c - system sleep (Hold) for the iOS 1.1.5 kernel on the 3DS.
 *
 * iOS sleeps like this (KERNEL.BIN):
 *   SpringBoard locks -> IOPMrootDomain::sleepSystem (C015DF7C) -> every
 *   driver's setPowerState(0) -> IOPlatformExpert::sleepKernel (C014B1B0)
 *   -> IOCPUSleepKernel (C014CA2C) -> AppleARMCPU::haltCPU (C0157D90):
 *   setCPUState(2), processor_exit -> processor_offline -> cpu_sleep
 *   (C005A7F4): saves state, plants the reset/resume vector, then
 *   PE_cpu_machine_quiesce -> AppleARMCPU::quiesceCPU -> platform quiesce
 *   actions -> ml_arm_sleep, which cleans the cache and spins on `b .`
 *   until the PMU cuts power.  A button makes the PMU power the SoC up
 *   again; the boot ROM / iBoot jump to the resume vector and the kernel
 *   continues the sleeping thread after haltCPU.
 *
 * None of that works here: cpu_sleep panics "cpu_sleep(): no support"
 * unless boot-arg sleep_test is set (C005A6D4), nothing can cut power, and
 * a reset-resume would re-run the kernel's start code underneath the HTL.
 * So the CPU never leaves: haltCPU's `bl processor_exit` is replaced by
 * a trap, and we sleep right there - bottom screen black, "SLEEP" on the
 * status rows, polling the 3DS buttons with everything else stopped.  A
 * wake button (START / SELECT / A) is latched as the iPod button that woke
 * it (GPIO interrupt 0x2E / 0x2D, the iPod's PMU-button wake), the CPU is
 * marked running again (IOCPU::setCPUState(3), as initCPU does on resume)
 * and haltCPU returns; IOCPUSleepKernel then unwinds into the normal wake:
 * drivers power on, SpringBoard shows the lock screen.
 *
 * A second trap marks IOPMrootDomain::sleepSystem, so the status row tells
 * "sleep requested but never reached the CPU" from "asleep".
 */

#include "pm.h"
#include "console.h"
#include "display.h"
#include "periph/input.h"

/* AppleARMCPU::haltCPU: `bl processor_exit` (Thumb, 4 bytes). */
#define HALT_PC         0xC0157DA4u
#define HALT_ORIG0      0xF6C8u
#define HALT_ORIG1      0xFF94u
/* IOPMrootDomain::sleepSystem + 2: `movs r3, #140`. */
#define SLEEPSYS_PC     0xC015DF7Eu
#define SLEEPSYS_ORIG   0x238Cu

#define UDF_HALT        0xDEA1u     /* Thumb-1 permanently undefined */
#define UDF_SLEEPSYS    0xDEA2u
#define T_NOP           0x46C0u     /* mov r8, r8 */

#define IOCPU_STATE_OFF 88          /* IOCPU::setCPUState: str r1, [r0, #88] */
#define CPU_RUNNING     3

#define PSR_N           (1u << 31)
#define PSR_Z           (1u << 30)
#define PSR_T           (1u << 5)

static char pm_state = 'A';         /* A awake, R requested, S asleep */
static u32 pm_requests, pm_sleeps;
static bool pm_patched;

/* ── Entry probes ─────────────────────────────────────────────────────
 * The first instruction of each function below (a Thumb `push` or an ARM
 * `stmdb sp!`) is replaced by an undefined instruction; the trap records
 * the call (and r1) and performs the push itself, so nothing else
 * changes.  The PM row shows the last calls: the whole power path from
 * the root domain down to the buttons, the panel and the PMU. */
typedef struct {
    u32  pc;
    u32  orig;              /* original instruction (Thumb: halfword) */
    char tag;               /* lower-case-free tags go to the history */
    bool arm;
    bool count_only;        /* frequent: counted, not in the history */
    u8   argreg;            /* register shown as the argument */
} probe_t;

static const probe_t probes[] = {
    { 0xC015DC3C, 0xB5F0,     'N', false, false, 1 },  /* rootDomain::receivePowerNotification(msg) */
    { 0xC015DF6C, 0xB580,     'S', false, false, 1 },  /* IORootParent::sleepSystem */
    { 0xC015DF5C, 0xB580,     'D', false, false, 1 },  /* IORootParent::dozeSystem */
    { 0xC015DF48, 0xB580,     'd', false, false, 1 },  /* IORootParent::sleepToDoze */
    { 0xC015DB6C, 0xB590,     'T', false, false, 1 },  /* rootDomain::tellChangeDown(state) */
    { 0xC015E7E4, 0xB590,     'C', false, false, 1 },  /* rootDomain::powerChangeDone(prev) */
    { 0xC015DF10, 0xB590,     'W', false, false, 1 },  /* rootDomain::wakeFromDoze */
    { 0xC015DC2C, 0xB580,     'U', false, false, 1 },  /* IORootParent::wakeSystem */
    { 0xC015E77C, 0xB590,     'I', false, false, 1 },  /* rootDomain::handleSleepTimerExpiration */
    { 0xC014B1B0, 0xB580,     'K', false, false, 1 },  /* IOPlatformExpert::sleepKernel */
    { 0xC03AB570, 0xE92D40F0, 'B', true, false, 1 },  /* AppleM68Buttons::setPowerState(state) */
    { 0xC0435788, 0xE92D40F0, 'L', true, false, 1 },  /* AppleMerlotLCD::_lcdEnable(on) */
    { 0xC047FF8C, 0xE92D40F0, 'Z', true, false, 1 },  /* ApplePCF50635PMU "pmu go stdby" */
    { 0xC0367CF4, 0xB5F0,     'E', false, true, 3 },  /* IOHIDEventService key dispatch
                                                     * (AppleM68Buttons vtable 0x388), r3 = page */
    { 0xC013A534, 0xB5F0,     'A', false, true, 1 },  /* IOService::activityTickle */
};
#define NPROBES (sizeof(probes) / sizeof(probes[0]))
#define UDF_PROBE_T     0xDEB0u             /* + index */
#define UDF_PROBE_A     0xE7F000F0u         /* ARM udf #imm16: imm in 19:8, 3:0 */

static u8 probe_hits[NPROBES];
static u32 probe_arg[NPROBES];
#define HIST 8
static struct { char tag; u8 arg; } hist[HIST];
static u32 hist_n;

static void pm_note(char tag, u32 arg) {
    hist[hist_n % HIST].tag = tag;
    hist[hist_n % HIST].arg = (u8)arg;
    hist_n++;
}

/* Banked SP/LR of the trapped mode (USR via SYS), read and write. */
static void bank_get(u32 mode, u32 *sp, u32 *lr) {
    u32 cpsr, s, l;
    if (mode == 0x10)
        mode = 0x1F;
    __asm__ volatile("mrs %0, cpsr" : "=r"(cpsr));
    u32 target = (cpsr & ~0x1Fu) | mode;
    __asm__ volatile("msr cpsr_c, %3\n"
                     "mov %0, sp\n"
                     "mov %1, lr\n"
                     "msr cpsr_c, %2\n"
                     : "=&r"(s), "=&r"(l) : "r"(cpsr), "r"(target) : "memory");
    *sp = s;
    *lr = l;
}

static void bank_set_sp(u32 mode, u32 sp) {
    u32 cpsr;
    if (mode == 0x10)
        mode = 0x1F;
    __asm__ volatile("mrs %0, cpsr" : "=r"(cpsr));
    u32 target = (cpsr & ~0x1Fu) | mode;
    __asm__ volatile("msr cpsr_c, %1\n"
                     "mov sp, %2\n"
                     "msr cpsr_c, %0\n"
                     :: "r"(cpsr), "r"(target), "r"(sp) : "memory");
}

/* Push regs (bit n = rn, r0-r12 from the frame, bit 14 = banked lr). */
static void emulate_push(u32 *frame, u32 spsr, u32 list) {
    u32 mode = spsr & 0x1F, sp, lr, n = 0;
    bank_get(mode, &sp, &lr);
    for (u32 r = 0; r < 16; r++)
        if (list & (1u << r))
            n++;
    u32 a = sp - 4 * n;
    volatile u32 *m = (volatile u32 *)a;
    for (u32 r = 0; r < 16; r++) {
        if (!(list & (1u << r)))
            continue;
        *m++ = r == 14 ? lr : r < 13 ? frame[r] : 0;
    }
    bank_set_sp(mode, a);
}

static bool probe_trap(u32 *frame, u32 spsr, u32 pc, bool thumb) {
    for (u32 i = 0; i < NPROBES; i++) {
        const probe_t *p = &probes[i];
        if (p->pc != pc || p->arm == thumb)
            continue;
        probe_hits[i]++;
        probe_arg[i] = frame[p->argreg];
        if (!p->count_only)
            pm_note(p->tag, frame[p->argreg]);
        if (thumb) {
            u32 list = (p->orig & 0xFF) | ((p->orig & 0x100) ? 1u << 14 : 0);
            emulate_push(frame, spsr, list);
            frame[13] = pc + 2;
        } else {
            emulate_push(frame, spsr, p->orig & 0xFFFF);
            frame[13] = pc + 4;
        }
        return true;
    }
    return false;
}

bool pm_patch_kernel(void) {
    volatile u16 *h = (volatile u16 *)HALT_PC;
    volatile u16 *s = (volatile u16 *)SLEEPSYS_PC;
    if (h[0] != HALT_ORIG0 || h[1] != HALT_ORIG1 || s[0] != SLEEPSYS_ORIG) {
        console_print("[PM] sleep path not as expected, left alone\n");
        return false;
    }
    h[0] = UDF_HALT;
    h[1] = T_NOP;
    s[0] = UDF_SLEEPSYS;
    pm_patched = true;
    u32 n = 0;
    for (u32 i = 0; i < NPROBES; i++) {
        const probe_t *p = &probes[i];
        if (p->arm) {
            volatile u32 *w = (volatile u32 *)p->pc;
            if (*w != p->orig)
                continue;
            u32 imm = 0xA000 + i;               /* udf #imm: 19:8 imm[15:4], 3:0 imm[3:0] */
            *w = UDF_PROBE_A | (imm >> 4) << 8 | (imm & 0xF);
        } else {
            volatile u16 *w = (volatile u16 *)p->pc;
            if (*w != p->orig)
                continue;
            *w = (u16)(UDF_PROBE_T + i);
        }
        n++;
    }
    console_print("[PM] sleep traps: haltCPU, sleepSystem; probes ");
    console_print_hex(n);
    console_print("\n");
    return true;
}

static void pm_sleep(u32 *frame) {
    pm_sleeps++;
    pm_note('H', 0);
    pm_state = 'S';
    display_sleep_blank();
    console_print("[PM] asleep: START/SELECT/A wakes\n");
    extern void htl_status_redraw(void);
    htl_status_redraw();

    /* wait for the buttons to be up (the Hold press that started this),
     * then for a press */
    u32 pad;
    while (input_wake_buttons(input_pad_now()))
        ;
    while (!(pad = input_wake_buttons(input_pad_now())))
        ;

    pm_state = 'A';
    input_wake(pad);                     /* the button that woke the iPod */
    u32 cpu = frame[4];                  /* haltCPU's r4 = this */
    *(volatile u32 *)(cpu + IOCPU_STATE_OFF) = CPU_RUNNING;
    console_print("[PM] wake\n");
    htl_status_redraw();
}

u32 htl_kern_undef_hook(u32 *frame, u32 spsr) {
    if (!pm_patched)
        return 0;
    bool thumb = spsr & PSR_T;
    u32 pc = frame[13] - (thumb ? 2 : 4);    /* LR_und = PC + 2 / + 4 */
    if (probe_trap(frame, spsr, pc, thumb))
        return spsr;
    if (!thumb)
        return 0;
    if (pc == SLEEPSYS_PC) {
        /* emulate movs r3, #140: r3, N = Z = 0 */
        pm_requests++;
        pm_note('R', frame[1]);
        if (pm_state == 'A')
            pm_state = 'R';
        frame[3] = 140;
        frame[13] = pc + 2;
        return spsr & ~(PSR_N | PSR_Z);
    }
    if (pc == HALT_PC) {
        pm_sleep(frame);
        frame[13] = pc + 4;              /* past udf + nop: pop {r4,r7,pc} */
        return spsr;
    }
    return 0;
}

bool pm_asleep(void) {
    return pm_state == 'S';
}

/* " P<state><requests><sleeps>", 7 columns. */
char *pm_status(char *p) {
    static const char h[] = "0123456789ABCDEF";
    *p++ = ' ';
    *p++ = 'P';
    *p++ = pm_patched ? pm_state : '-';
    *p++ = h[(pm_requests >> 4) & 0xF];
    *p++ = h[pm_requests & 0xF];
    *p++ = h[(pm_sleeps >> 4) & 0xF];
    *p++ = h[pm_sleeps & 0xF];
    return p;
}

static char *hx2(char *p, u32 v, u32 d) {
    static const char h[] = "0123456789ABCDEF";
    while (d--)
        *p++ = h[(v >> (4 * d)) & 0xF];
    return p;
}

/* "PM <state> <tag><arg>... #<n> E<hid> A<tickles>": the last 8
 * power-path calls, newest last (arg = low byte of r1: the state for
 * B/L/T/C, the message for N), then counts of HID key dispatches and
 * activityTickle calls (wrapping at 0x100). */
void pm_trace_line(char *buf) {
    char *p = buf;
    *p++ = 'P'; *p++ = 'M';
    *p++ = ' '; *p++ = pm_patched ? pm_state : '-';
    u32 from = hist_n > HIST ? hist_n - HIST : 0;
    for (u32 i = from; i < hist_n; i++) {
        *p++ = ' ';
        *p++ = hist[i % HIST].tag;
        u32 a = hist[i % HIST].arg;
        p = hx2(p, a, 2);
    }
    *p++ = ' '; *p++ = '#';
    p = hx2(p, hist_n, 3);
    for (u32 i = 0; i < NPROBES; i++)          /* count-only probes */
        if (probes[i].count_only) {
            *p++ = ' ';
            *p++ = probes[i].tag;
            p = hx2(p, probe_hits[i], 2);
        }
    *p = 0;
}
