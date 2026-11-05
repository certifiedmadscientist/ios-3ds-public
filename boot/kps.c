/*
 * kps.c — "ps" for the stall dump plus a sampled PC profile.
 *
 * Kernel structures are read through the kernel's own page tables (VA ->
 * PA walk) and then through the linear window, so a stale or garbage
 * pointer yields 0, never a fault.  Offsets come from kernel/KERNEL.BIN
 * (iPhone OS 1.x xnu, ARM Thumb):
 *
 *   _allproc 0xC0259C2C (LIST_HEAD), _zombproc 0xC0263F40,
 *   _CurrentThread 0xC01D0D34, _disableConsoleOutput 0xC01D1C98,
 *   km _initialized 0xC01BE220, _cons_ops_index 0xC01AD044
 *   proc:    p_list.le_next +0x00, p_stat (s8) +0x20, p_pid +0x24,
 *            p_comm +0xEB, task +0x138                    (task_for_pid)
 *   task:    threads queue head +0x28, bsd_info +0x178    (get_signalact)
 *   thread:  wait_queue +0x0C, wait_event +0x10, continuation +0x44,
 *            state +0x5C (W1 S2 R4 U8 T10 T2 20 I80), task_threads.next
 *            +0x1AC, user saved state ptr +0x28C (pc +0x3C, cpsr +0x40),
 *            kernel ctx ptr +0x29C ({r4-r11,ip,sp,lr} at +0x10),
 *            task +0x2FC, uthread +0x3B4                  (thread_unblock,
 *            wait_queue_assert_wait64_locked, Switch_context)
 *   uthread: uu_wchan +0x9C, uu_wmesg +0xA0                (_sleep)
 */

#include "types.h"
#include "console.h"

u32 htl_kva_pa(u32 va);          /* htl.c: table walk, ~0 if unmapped */
u32 nand_adm_idle(void);         /* periph/nand.c */
void uart_tx_counts(u32 *c);     /* periph/uart.c */

/* ── safe kernel reads ─────────────────────────────────────────────── */

static u32 kr32(u32 va) {
    if (va < 0xC0000000u || (va & 3))
        return 0;
    u32 pa = htl_kva_pa(va);
    if (pa - 0x21000000u >= 0x07000000u)    /* FCRAM linear window only */
        return 0;
    return *(volatile u32 *)(pa + 0x9F000000u);
}

static u32 kr8(u32 va) {
    return (kr32(va & ~3u) >> ((va & 3) * 8)) & 0xFF;
}

/* ── output ────────────────────────────────────────────────────────── */

static char *hx(char *p, u32 v, u32 digits) {
    static const char h[] = "0123456789ABCDEF";
    for (int i = (int)digits - 1; i >= 0; i--)
        *p++ = h[(v >> (i * 4)) & 0xF];
    return p;
}

static char *str(char *p, const char *q) {
    while (*q)
        *p++ = *q++;
    return p;
}

/* Copy a kernel string, padded to width with spaces. */
static char *kstr(char *p, u32 va, u32 width) {
    u32 i = 0;
    if (va)
        for (; i < width; i++) {
            u32 c = kr8(va + i);
            if (!c)
                break;
            *p++ = (c >= 0x20 && c < 0x7F) ? (char)c : '?';
        }
    for (; i < width; i++)
        *p++ = ' ';
    return p;
}

static void line(char *b, char *p) {
    *p++ = '\n';
    *p = 0;
    console_print(b);
}

/* ── ps ────────────────────────────────────────────────────────────── */

#define TH_WAIT   0x01
#define TH_SUSP   0x02
#define TH_RUN    0x04
#define TH_UNINT  0x08
#define TH_TERM   0x10
#define TH_IDLE   0x80

static char *state(char *p, u32 s) {
    /* two letters: R(un)/W(ait)/D(isk-style uninterruptible wait)/S(usp),
     * then T(erminating)/I(dle)/blank */
    *p++ = (s & TH_WAIT) ? ((s & TH_UNINT) ? 'D' : 'W')
         : (s & TH_RUN) ? 'R' : (s & TH_SUSP) ? 'S' : '?';
    *p++ = (s & TH_TERM) ? 'T' : (s & TH_IDLE) ? 'I'
         : ((s & TH_SUSP) && (s & (TH_WAIT | TH_RUN))) ? 's' : ' ';
    return p;
}

static u32 n_threads, n_runnable, run_thr, run_pid;

static u32 pid_of(u32 proc) {
    return proc ? kr32(proc + 0x24) : 0xFF;
}

/* One line per process:
 *   "PP comm.... SS+n wchan... wmesg...|kPC..... uPC....."
 * for the first thread whose state isn't plain running, else the first
 * thread; wmesg if the BSD sleep set one, else the kernel resume PC. */
static void ps_proc(u32 proc, u32 cur) {
    char b[64], *p = b;
    u32 task = kr32(proc + 0x138);
    u32 head = task ? task + 0x28 : 0;
    u32 first = 0, pick = 0, n = 0, cur_here = 0;

    for (u32 th = head ? kr32(head) : 0; th && th != head && n < 32;
         th = kr32(th + 0x1AC)) {
        u32 s = kr32(th + 0x5C);
        n++;
        n_threads++;
        if ((s & (TH_RUN | TH_WAIT | TH_IDLE)) == TH_RUN) {
            n_runnable++;
            if (!run_thr) {
                run_thr = th;
                run_pid = kr32(proc + 0x24);
            }
        }
        if (!first)
            first = th;
        if (!pick && (s & TH_WAIT))
            pick = th;
        if (th == cur)
            cur_here = 1;
    }
    if (!pick)
        pick = first;

    p = hx(p, kr32(proc + 0x24), 2);
    *p++ = cur_here ? '*' : ' ';
    p = kstr(p, proc + 0xEB, 9);
    *p++ = '^';
    p = hx(p, pid_of(kr32(proc + 0x30)), 2);   /* parent pid */
    *p++ = ' ';
    if (!pick) {
        p = str(p, "no threads task ");
        p = hx(p, task, 8);
        line(b, p);
        return;
    }
    u32 s = kr32(pick + 0x5C);
    p = state(p, s);
    *p++ = n > 15 ? '+' : "0123456789ABCDEF"[n];
    *p++ = ' ';
    p = hx(p, kr32(pick + 0x10), 8);       /* wait_event */
    *p++ = ' ';
    u32 ut = kr32(pick + 0x3B4);
    u32 wmesg = ut ? kr32(ut + 0xA0) : 0;
    if ((s & TH_WAIT) && wmesg) {
        p = kstr(p, wmesg, 8);
    } else {
        u32 kctx = kr32(pick + 0x29C);
        *p++ = 'k';
        p = hx(p, kctx ? kr32(kctx + 0x10 + 0x28) : 0, 8);
    }
    *p++ = ' ';
    u32 uss = kr32(pick + 0x28C);
    p = hx(p, uss ? kr32(uss + 0x3C) : 0, 8);
    line(b, p);
}

/* Console path state: kmputc drops output while disableConsoleOutput is
 * set or km isn't initialized; tx counts UART bytes we actually saw. */
static void ps_console(void) {
    char b[64], *p = b;
    u32 tx[5];
    uart_tx_counts(tx);
    p = str(p, "[PS] con dis"); p = hx(p, kr32(0xC01D1C98), 1);
    p = str(p, " km"); p = hx(p, kr32(0xC01BE220), 1);
    p = str(p, " ops"); p = hx(p, kr32(0xC01AD044), 1);
    p = str(p, " tx"); p = hx(p, tx[0], 6);
    p = str(p, " other"); p = hx(p, tx[1] + tx[2] + tx[3] + tx[4], 4);
    line(b, p);
}

/* ── symbols (ksyms.S, generated from KERNEL.BIN) ──────────────────── */

extern const u32 ksym_count, ksym_addr[], ksym_name[];
extern const char ksym_str[];

/* "name+off" for kernel text; "K<pc>" for (unsymbolized) prelinked kexts. */
static char *sym(char *p, u32 pc, u32 room) {
    pc &= ~1u;
    if (pc - 0xC0009000u < 0x159C00u && ksym_count) {
        u32 lo = 0, hi = ksym_count;
        while (hi - lo > 1) {
            u32 mid = (lo + hi) / 2;
            if (ksym_addr[mid] <= pc)
                lo = mid;
            else
                hi = mid;
        }
        if (ksym_addr[lo] <= pc) {
            const char *n = ksym_str + ksym_name[lo];
            u32 off = pc - ksym_addr[lo];
            u32 max = room > 6 ? room - 6 : 1;   /* leave "+XXXXX" */
            for (u32 i = 0; *n && i < max; i++)
                *p++ = *n++;
            *p++ = '+';
            return hx(p, off, off > 0xFFF ? 5 : 3);
        }
    }
    *p++ = pc - 0xC029B000u < 0x2CA000u ? 'K' : '?';
    return hx(p, pc, 8);
}

/* Kernel backtrace of a blocked thread from its Switch_context save area
 * ({r4-r11, ip, sp, lr} at ctx+0x10; r7 is the frame pointer, frames are
 * {saved r7, saved lr}), then the words around its wait event. */
static void ps_map(u32 hdr);

static void ps_backtrace(u32 th, u32 frames) {
    char b[64], *p;
    u32 ctx = kr32(th + 0x29C);
    if (!ctx)
        return;
    u32 fp = kr32(ctx + 0x10 + 3 * 4);
    u32 pc = kr32(ctx + 0x10 + 10 * 4);
    for (u32 i = 0; i < frames && pc; i++) {
        p = b;
        p = str(p, "  "); p = hx(p, i, 1); *p++ = ' ';
        p = sym(p, pc, 40);
        line(b, p);
        if (!fp || (fp & 3))
            break;
        u32 nfp = kr32(fp);
        pc = kr32(fp + 4);
        if (nfp && nfp <= fp)                /* stacks grow down */
            break;
        fp = nfp;
    }
    u32 ev = kr32(th + 0x10) & ~3u;
    if (kr32(ev + 12) == 0x40000000u)
        ps_map(ev);
    for (u32 r = 0; r < 2; r++) {
        p = b;
        p = str(p, "  @"); p = hx(p, ev - 0x10 + r * 0x10, 8);
        for (u32 k = 0; k < 4; k++) {
            *p++ = ' ';
            p = hx(p, kr32(ev - 0x10 + r * 0x10 + k * 4), 8);
        }
        line(b, p);
    }
}

/* The event is a vm_map's hdr (vm_map_entry_wait: links {prev, next,
 * start, end}, end 0x40000000 for a user map): list its entries that are
 * wired or in transition, which is what vm_map_delete waits on.
 * Entry: links +0 {prev,next,start,end}, object +0x10, flags byte +0x1C
 * (4 in_transition, 8 needs_wakeup, 2 is_sub_map), wired_count u16 +0x20,
 * user_wired_count u16 +0x22. */
static void ps_map(u32 hdr) {
    char b[64], *p;
    u32 n = 0, shown = 0;
    for (u32 e = kr32(hdr + 4); e && e != hdr && n < 400; e = kr32(e + 4)) {
        n++;
        u32 w = kr32(e + 0x20), fl = kr32(e + 0x1C) & 0xFF;
        if (!(w & 0xFFFF) && !(w >> 16) && !(fl & 4))
            continue;
        if (shown++ >= 8)
            continue;
        p = b;
        p = str(p, "  E"); p = hx(p, kr32(e + 8), 8);
        *p++ = '-'; p = hx(p, kr32(e + 12), 8);
        p = str(p, " w"); p = hx(p, w & 0xFFFF, 2);
        p = str(p, " u"); p = hx(p, w >> 16, 2);
        p = str(p, " f"); p = hx(p, fl, 2);
        p = str(p, " o"); p = hx(p, kr32(e + 0x10), 8);
        line(b, p);
    }
    p = b;
    p = str(p, "  map entries "); p = hx(p, n, 3);
    p = str(p, " wired/transit "); p = hx(p, shown, 3);
    line(b, p);
}

/* Every thread queued on one of the 59 global wait queues (event-hashed,
 * 16 bytes each: type/lock, then queue head at +8), kernel_task's own
 * waiters only counted.  '!' = thread->wait_queue isn't this bucket,
 * '#' = the event doesn't hash here (|ev| % 59). */
static void ps_waitq(void) {
    char b[64], *p;
    u32 ktask = kr32(0xC01D25C4), kwait = 0, kd = 0, total = 0;

    for (u32 k = 0; k < 59; k++) {
        u32 wq = 0xC0259800u + k * 16, head = wq + 8, n = 0;
        if (kr32(wq + 4)) {                  /* interlock held? */
            p = b;
            p = str(p, "[WQ]"); p = hx(p, k, 2);
            p = str(p, " lock "); p = hx(p, kr32(wq + 4), 8);
            line(b, p);
        }
        for (u32 th = kr32(head); th && th != head && n < 24; th = kr32(th)) {
            n++;
            total++;
            u32 task = kr32(th + 0x2FC);
            bool dwait = (kr32(th + 0x5C) & (TH_WAIT | TH_UNINT)) ==
                         (TH_WAIT | TH_UNINT);
            if (task == ktask) {
                kwait++;
                if (!dwait || kd++ >= 4)    /* kernel D waiters: first 4 */
                    continue;
            }
            u32 ev = kr32(th + 0x10);
            u32 aev = (s32)ev < 0 ? ~ev : ev;
            p = b;
            p = str(p, "[WQ]"); p = hx(p, k, 2);
            *p++ = kr32(th + 0x0C) == wq ? ' ' : '!';
            *p++ = aev % 59 == k ? ' ' : '#';
            p = hx(p, th, 8);
            *p++ = ' ';
            p = hx(p, pid_of(task ? kr32(task + 0x178) : 0), 2);
            *p++ = ' ';
            p = state(p, kr32(th + 0x5C));
            *p++ = ' ';
            p = hx(p, ev, 8);
            *p++ = ' ';
            u32 ut = kr32(th + 0x3B4);
            u32 wmesg = ut ? kr32(ut + 0xA0) : 0;
            if (wmesg) {
                p = kstr(p, wmesg, 8);
            } else {
                *p++ = 'c';                  /* continuation */
                p = hx(p, kr32(th + 0x44), 8);
            }
            line(b, p);
            if (dwait)
                ps_backtrace(th, task == ktask ? 6 : 12);
        }
    }
    p = b;
    p = str(p, "[WQ] "); p = hx(p, total, 3);
    p = str(p, " waiting, kernel_task "); p = hx(p, kwait, 3);
    line(b, p);
}

/* Zombies: pid, comm, parent pid, exit status (+0x128), lflag (+0x218). */
static void ps_zombies(void) {
    char b[64], *p;
    u32 z = 0;
    for (u32 proc = kr32(0xC0263F40); proc && z < 200; proc = kr32(proc)) {
        if (z++ >= 6)
            continue;
        p = b;
        p = str(p, "Z "); p = hx(p, kr32(proc + 0x24), 2);
        *p++ = ' ';
        p = kstr(p, proc + 0xEB, 9);
        *p++ = '^';
        p = hx(p, pid_of(kr32(proc + 0x30)), 2);
        p = str(p, " @"); p = hx(p, proc, 8);
        p = str(p, " x"); p = hx(p, kr32(proc + 0x128), 4);
        p = str(p, " L"); p = hx(p, kr32(proc + 0x218), 3);
        line(b, p);
    }
}

void kps_dump(u32 idle_ticks) {
    char b[64], *p = b;
    u32 cur = kr32(0xC01D0D34);

    n_threads = n_runnable = run_thr = run_pid = 0;
    u32 cpu = cur ? kr32(cur + 0x2A0) : 0;
    p = str(p, "[PS] idle "); p = hx(p, idle_ticks, 5);
    p = str(p, " cur "); p = hx(p, cur, 8);
    p = str(p, " ast "); p = hx(p, cpu ? kr32(cpu + 0x34) : 0xFFFFFFFF, 8);
    line(b, p);

    u32 n = 0;
    for (u32 proc = kr32(0xC0259C2C); proc && n < 40; proc = kr32(proc)) {
        ps_proc(proc, cur);
        n++;
    }
    /* proc addresses, to compare with wait events ("pid=addr") */
    p = b;
    n = 0;
    for (u32 proc = kr32(0xC0259C2C); proc && n < 40; proc = kr32(proc)) {
        p = hx(p, kr32(proc + 0x24), 2);
        *p++ = '=';
        p = hx(p, proc, 8);
        *p++ = ' ';
        if (++n % 4 == 0) {
            line(b, p - 1);
            p = b;
        }
    }
    if (p != b)
        line(b, p - 1);
    ps_zombies();
    ps_waitq();
    u32 z = 0;
    for (u32 proc = kr32(0xC0263F40); proc && z < 200; proc = kr32(proc))
        z++;

    p = b;
    p = str(p, "[PS] "); p = hx(p, n, 2);
    p = str(p, " procs "); p = hx(p, z, 2);
    p = str(p, " zomb "); p = hx(p, n_threads, 3);
    p = str(p, " thr "); p = hx(p, n_runnable, 2);
    p = str(p, " run "); p = hx(p, run_thr, 8);
    *p++ = '/'; p = hx(p, run_pid, 2);
    line(b, p);
    ps_console();
}

/* ── Blocked IOKit drivers ───────────────────────────────────────────────
 * IOService::waitForService (0xC01351DE-0xC0135294) is called with no
 * timeout by AppleARMFunction::withProvider (0xC0158084 symbol, 0xC015816C
 * string), which every driver uses to resolve its function-* properties: it
 * blocks until a service publishes IOFunctionParent<phandle>.  A kernel
 * thread asleep under either is a driver whose start() cannot finish.  Each
 * scan walks kernel_task's threads; for those waiting inside waitForService
 * the first frame in the prelinked kext range is the driver's call site
 * (for AppleMerlotLCD, the blx at 0xC0435508/14/24/34/... says which
 * function-* it is waiting for).  Call sites seen are kept (sticky). */

#define WFS_LO      0xC01351DEu
#define WFS_HI      0xC0135294u
#define AFN_LO      0xC0158084u
#define AFN_HI      0xC0158196u
#define KEXT_LO     0xC029B000u
#define KEXT_HI     0xC0601000u
#define IOK_SLOTS   8

/* kind: 0 not blocked in the last scan, 1 blocked in waitForService,
 * 2 blocked in some other wait with a driver on the stack. */
static struct { u32 pc; u32 kind; } iok[IOK_SLOTS];
static u32 iok_n_wfs, iok_n_dwait, iok_scans;

static void iok_note(u32 pc, u32 kind) {
    u32 slot = IOK_SLOTS;
    for (u32 i = 0; i < IOK_SLOTS; i++)
        if (iok[i].pc == pc) {
            slot = i;
            break;
        }
    for (u32 i = 0; slot == IOK_SLOTS && i < IOK_SLOTS; i++)
        if (!iok[i].pc)
            slot = i;
    for (u32 i = 0; slot == IOK_SLOTS && i < IOK_SLOTS; i++)
        if (!iok[i].kind)               /* replace a stale entry */
            slot = i;
    if (slot < IOK_SLOTS) {
        if (iok[slot].pc != pc) {
            iok[slot].pc = pc;
            iok[slot].kind = 0;
        }
        if (!iok[slot].kind)
            iok[slot].kind = kind;
    }
}

static void iok_scan(void) {
    u32 ktask = kr32(0xC01D25C4);
    u32 head = ktask ? ktask + 0x28 : 0;
    u32 wfs = 0, dwait = 0, n = 0;

    for (u32 i = 0; i < IOK_SLOTS; i++)
        iok[i].kind = 0;
    for (u32 th = head ? kr32(head) : 0; th && th != head && n < 128;
         th = kr32(th + 0x1AC)) {
        n++;
        u32 st = kr32(th + 0x5C);
        if (!(st & TH_WAIT))
            continue;
        if (st & TH_UNINT)
            dwait++;
        u32 ctx = kr32(th + 0x29C);
        if (!ctx)
            continue;
        u32 fp = kr32(ctx + 0x10 + 3 * 4);
        u32 pc = kr32(ctx + 0x10 + 10 * 4);
        bool in_wfs = false;
        u32 kext = 0, outer = 0;
        for (u32 f = 0; f < 24 && pc; f++) {
            u32 a = pc & ~1u;
            bool here = (a >= WFS_LO && a < WFS_HI) || (a >= AFN_LO && a < AFN_HI);
            if (a >= WFS_LO && a < WFS_HI)
                in_wfs = true;
            if (in_wfs && !here && !outer)
                outer = pc;
            if (!kext && !here && a >= KEXT_LO && a < KEXT_HI)
                kext = pc;
            if (!fp || (fp & 3))
                break;
            u32 nfp = kr32(fp);
            pc = kr32(fp + 4);
            if (nfp && nfp <= fp)
                break;
            fp = nfp;
        }
        if (in_wfs)
            wfs++;
        if (kext)
            iok_note(kext, in_wfs ? 1 : 2);
        else if (in_wfs && outer)
            iok_note(outer, 1);         /* kernel caller of waitForService */
    }
    iok_n_wfs = wfs;
    iok_n_dwait = dwait;
    iok_scans++;
}

/* Two rows: "W<threads in waitForService> D<kernel D waiters>" and the
 * driver call sites seen asleep, 4 per row (the first row starts with the
 * counts): <call site><*=in waitForService now, +=other wait now,
 * .=seen earlier>. */
void kps_iok_line(char *buf, u32 now, char *buf2) {
    static u32 last;
    if (now - last >= 100 || !last) {       /* ~1 s */
        last = now ? now : 1;
        iok_scan();
    }
    static const char mark[3] = { '.', '*', '+' };
    char *p = buf;
    *p++ = 'W'; p = hx(p, iok_n_wfs, 2);
    p = str(p, " D"); p = hx(p, iok_n_dwait, 2);
    for (u32 i = 0; i < 4 && iok[i].pc; i++) {
        *p++ = ' ';
        p = hx(p, iok[i].pc, 8);
        *p++ = mark[iok[i].kind];
    }
    *p = 0;
    p = buf2;
    for (u32 i = 4; i < IOK_SLOTS && iok[i].pc; i++) {
        if (p != buf2)
            *p++ = ' ';
        p = hx(p, iok[i].pc, 8);
        *p++ = mark[iok[i].kind];
    }
    *p = 0;
}

/* ── PC profile (one sample per 100 Hz heartbeat) ──────────────────── */

#define TOP 16
static struct { u32 pc, n; } top[TOP];
static u32 s_user, s_idle, s_kern, s_ours, s_emul, s_total;
u32 kps_after_emul;     /* set by htl_kern_abort after emulating MMIO */
static char prof_line[52];

static bool is_idle(u32 pc) {
    return pc - 0xC005A920u < 0x128u ||     /* _cpu_idle_exit.._cpu_signal */
           pc - 0xC0022BA8u < 0x2ACu ||     /* _idle_thread */
           pc - 0xC0061038u < 0x18u;        /* _machine_idle */
}

static char *pct(char *p, u32 v, u32 t) {
    u32 x = v * 100 / t;
    if (x > 99)
        x = 99;
    *p++ = (char)('0' + x / 10);
    *p++ = (char)('0' + x % 10);
    return p;
}

static void publish(void) {
    u32 best = 0;
    for (u32 i = 1; i < TOP; i++)
        if (top[i].n > top[best].n)
            best = i;
    u32 t = s_total ? s_total : 1;
    char *p = prof_line;
    p = str(p, "P% u"); p = pct(p, s_user, t);
    p = str(p, " i"); p = pct(p, s_idle, t);
    p = str(p, " k"); p = pct(p, s_kern, t);
    p = str(p, " h"); p = pct(p, s_ours, t);
    p = str(p, " e"); p = pct(p, s_emul, t);
    p = str(p, " top "); p = hx(p, top[best].pc, 8);
    *p++ = 'x'; p = hx(p, top[best].n, 3);
    *p = 0;
    for (u32 i = 0; i < TOP; i++)
        top[i].n = top[i].pc = 0;
    s_user = s_idle = s_kern = s_ours = s_emul = s_total = 0;
}

void kps_sample(u32 pc, u32 spsr) {
    s_total++;
    bool emul = kps_after_emul;
    kps_after_emul = 0;
    /* An IRQ pending while we emulated an MMIO access (IRQs masked) is
     * taken on the instruction after it, so that sample is really the
     * emulation: count it separately instead of as kernel time there. */
    if (emul && (spsr & 0x1F) != 0x10 && !is_idle(pc)) {
        s_emul++;
    } else if ((spsr & 0x1F) == 0x10) {
        s_user++;
    } else if (pc >= 0xC0800000u && pc < 0xC0C00000u) {
        s_ours++;
    } else if (is_idle(pc)) {
        s_idle++;
    } else {
        s_kern++;
        /* space-saving top-k: bump a match, else replace the smallest */
        u32 lo = 0;
        for (u32 i = 0; i < TOP; i++) {
            if (top[i].pc == pc && top[i].n) {
                top[i].n++;
                goto done;
            }
            if (top[i].n < top[lo].n)
                lo = i;
        }
        top[lo].pc = pc;
        top[lo].n++;
    }
done:
    if (s_total >= 500)                     /* 5 s windows */
        publish();
}

/* Status row: last profile window.  Also drives the ps trigger: once ADM
 * has been idle 5 s after activity, then every 30 s while it stays idle. */
void kps_status_line(char *buf, u32 now) {
    static bool armed = true;
    static u32 last;
    u32 idle = nand_adm_idle();

    if (idle < 500) {
        armed = true;
    } else if (armed || now - last >= 3000) {
        armed = false;
        last = now;
        kps_dump(idle);
    }

    const char *s = prof_line[0] ? prof_line : "P (sampling)";
    while ((*buf++ = *s++))
        ;
}

/* ── SpringBoard rows ──────────────────────────────────────────────────
 * Row a: "SB<pid> <threads> <state><wait event> <kernel resume|wmesg>
 *         u<user PC>" for its main (first) thread;
 * row b: "SB D<thread#> <state><event> <kernel resume>" for the first
 *         other thread blocked uninterruptibly in the kernel, if any.
 * Tells "idle in mach_msg, no events arriving" from "stuck in a call". */
static char *thread_brief(char *p, u32 th, u32 room) {
    u32 s = kr32(th + 0x5C);
    p = state(p, s);
    p = hx(p, kr32(th + 0x10), 8);           /* wait event */
    *p++ = ' ';
    u32 ut = kr32(th + 0x3B4);
    u32 wmesg = ut ? kr32(ut + 0xA0) : 0;
    if ((s & TH_WAIT) && wmesg)
        return kstr(p, wmesg, 8);
    u32 kctx = kr32(th + 0x29C);
    u32 pc = kctx ? kr32(kctx + 0x10 + 10 * 4) : 0;   /* saved lr */
    return pc ? sym(p, pc, room) : hx(p, kr32(th + 0x44), 8);
}

void kps_sb_lines(char *a, char *b) {
    char *p;
    u32 sb = 0;
    a[0] = b[0] = 0;
    for (u32 proc = kr32(0xC0259C2C), n = 0; proc && n < 64; proc = kr32(proc), n++) {
        char c[12], *q = c;
        q = kstr(q, proc + 0xEB, 11);
        *q = 0;
        const char *want = "SpringBoard";
        u32 i = 0;
        while (want[i] && c[i] == want[i])
            i++;
        if (!want[i]) {
            sb = proc;
            break;
        }
    }
    p = a;
    *p++ = 'S'; *p++ = 'B';
    if (!sb) {
        p = str(p, " not running");
        *p = 0;
        return;
    }
    u32 task = kr32(sb + 0x138), head = task ? task + 0x28 : 0, n = 0, main = 0, dth = 0, di = 0;
    for (u32 th = head ? kr32(head) : 0; th && th != head && n < 64; th = kr32(th + 0x1AC)) {
        if (!main)
            main = th;
        else if (!dth && (kr32(th + 0x5C) & (TH_WAIT | TH_UNINT)) == (TH_WAIT | TH_UNINT)) {
            dth = th;
            di = n;
        }
        n++;
    }
    p = hx(p, kr32(sb + 0x24), 2);
    *p++ = ' ';
    p = hx(p, n, 2);
    *p++ = ' ';
    if (main) {
        p = thread_brief(p, main, 18);
        u32 uss = kr32(main + 0x28C);
        p = str(p, " u");
        p = hx(p, uss ? kr32(uss + 0x3C) : 0, 8);
    }
    *p = 0;
    p = b;
    p = str(p, "SB D");
    if (dth) {
        p = hx(p, di, 2);
        *p++ = ' ';
        p = thread_brief(p, dth, 28);
    } else {
        p = str(p, "-- none");
    }
    *p = 0;
}
