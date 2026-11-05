/*
 * PL192 Vectored Interrupt Controller emulation, a port of qemu-ios
 * hw/intc/pl192.c (the model that boots this kernel there).
 *
 *   VIC0 at 0x38E00000 (lines 0-31)  -> CPU IRQ / FIQ
 *   VIC1 at 0x38E01000 (lines 32-63) -> daisy-chained into VIC0
 *
 * Reading VECTADDR (0xF00) acknowledges the highest-priority IRQ and pushes
 * its priority as the current mask; writing it (EOI) pops the mask.  A VIC1
 * IRQ appears at VIC0 as the daisy input carrying VIC1's vector address.
 * FIQ-selected lines bypass vectoring.
 */

#include "vic.h"
#include "../console.h"

#define VIC_IRQSTATUS       0x000
#define VIC_FIQSTATUS       0x004
#define VIC_RAWINTR         0x008
#define VIC_INTSELECT       0x00C
#define VIC_INTENABLE       0x010
#define VIC_INTENCLEAR      0x014
#define VIC_SOFTINT         0x018
#define VIC_SOFTINTCLEAR    0x01C
#define VIC_PROTECTION      0x020
#define VIC_SWPRIORITYMASK  0x024
#define VIC_PRIORITYDAISY   0x028
#define VIC_VECTADDR        0xF00
/* PL190 registers some kernels touch; ignored as in qemu-ios */
#define PL190_VECTADDR      0x030
#define PL190_DEFVECTADDR   0x034
#define PL190_ITCR          0x300

#define NSRC        32
#define DAISY_IRQ   NSRC
#define NO_IRQ      (NSRC + 1)
#define PRIO_LEVELS 16

typedef struct vic vic_t;
struct vic {
    u32 rawintr, softint, intenable, intselect;
    u32 protection, sw_priority_mask, daisy_priority;
    u32 vect_addr[NSRC];
    u32 vect_priority[NSRC];
    u32 irq_status, fiq_status;
    u32 address;
    u32 current, current_highest, priority;
    u32 stack_i;
    u32 priority_stack[NSRC + 2];
    u32 irq_stack[NSRC + 2];
    /* daisy input (VIC0) and output (VIC1) */
    bool daisy_input;
    u32 daisy_vectaddr;
    vic_t *daisy_callback;
    vic_t *daisy;
    bool irq_out;           /* VIC0 only: CPU IRQ line */
};

static vic_t vic[2];
static u32 line_count[64];
static u32 n_ack, n_eoi;

static void vic_update(vic_t *s);

static void vic_raise_irq(vic_t *s) {
    if (!s->daisy) {
        s->irq_out = true;
        vic_kick();
        return;
    }
    s->daisy->daisy_vectaddr = s->address;
    s->daisy->daisy_callback = s;
    s->daisy->daisy_input = true;
    vic_update(s->daisy);
}

static void vic_lower_irq(vic_t *s) {
    if (!s->daisy) {
        s->irq_out = false;
        return;
    }
    s->daisy->daisy_input = false;
    vic_update(s->daisy);
}

static u32 vic_priority_sorter(vic_t *s) {
    u32 prio_irq[PRIO_LEVELS];
    for (int i = 0; i < PRIO_LEVELS; i++)
        prio_irq[i] = NO_IRQ;
    if (s->daisy_input)
        prio_irq[s->daisy_priority] = DAISY_IRQ;
    for (int i = NSRC - 1; i >= 0; i--)
        if (s->irq_status & (1u << i))
            prio_irq[s->vect_priority[i]] = (u32)i;
    for (int i = 0; i < PRIO_LEVELS; i++)
        if ((s->sw_priority_mask & (1u << i)) && prio_irq[i] <= DAISY_IRQ)
            return prio_irq[i];
    return NO_IRQ;
}

static void vic_update(vic_t *s) {
    u32 in = s->rawintr | s->softint;
    s->irq_status = in & s->intenable & ~s->intselect;
    s->fiq_status = in & s->intenable & s->intselect;

    if (!(s->irq_status || s->daisy_input)) {
        s->current_highest = NO_IRQ;
        vic_lower_irq(s);
        return;
    }
    s->current_highest = vic_priority_sorter(s);
    if (s->current_highest < NSRC)
        s->address = s->vect_addr[s->current_highest];
    else
        s->address = s->daisy_vectaddr;
    if (s->current_highest == s->current)
        return;
    if (s->current_highest < NSRC &&
        s->vect_priority[s->current_highest] >= s->priority)
        return;
    if (s->current_highest == DAISY_IRQ && s->daisy_priority >= s->priority)
        return;
    if (s->current_highest <= DAISY_IRQ)
        vic_raise_irq(s);
    else
        vic_lower_irq(s);
}

static void vic_mask_priority(vic_t *s) {
    if (s->stack_i >= NSRC || s->current > DAISY_IRQ)
        return;
    s->stack_i++;
    s->priority = s->current == DAISY_IRQ ? s->daisy_priority
                                          : s->vect_priority[s->current];
    s->priority_stack[s->stack_i] = s->priority;
    s->irq_stack[s->stack_i] = s->current;
}

static void vic_unmask_priority(vic_t *s) {
    if (s->stack_i < 1)
        return;
    s->stack_i--;
    s->priority = s->priority_stack[s->stack_i];
    s->current = s->irq_stack[s->stack_i];
}

static u32 vic_irq_ack(vic_t *s) {
    bool is_daisy = s->current_highest == DAISY_IRQ;
    u32 res = s->address;

    /* Nothing pending: the kernel's dispatch loop reads VECTADDR once more
     * to find it has drained the controller, and writes no EOI for that
     * read.  Pushing the priority stack here would leave it unbalanced and
     * mask lower-priority lines, so treat it as a plain read. */
    if (s->current_highest > DAISY_IRQ)
        return res;
    n_ack++;

    if (s->current_highest < NSRC)
        line_count[(s == &vic[1] ? 32 : 0) + s->current_highest]++;
    else if (is_daisy && s->daisy_callback &&
             s->daisy_callback->current_highest < NSRC)
        line_count[32 + s->daisy_callback->current_highest]++;

    s->current = s->current_highest;
    if (!s->daisy)
        s->irq_out = false;     /* nIRQ drops on acknowledge (PL192 TRM) */
    vic_mask_priority(s);
    if (is_daisy && s->daisy_callback) {
        s->daisy_callback->current = s->daisy_callback->current_highest;
        vic_mask_priority(s->daisy_callback);
    }
    vic_update(s);
    return res;
}

static void vic_irq_fin(vic_t *s) {
    bool is_daisy = s->current == DAISY_IRQ;
    n_eoi++;
    vic_unmask_priority(s);
    if (is_daisy && s->daisy_callback) {
        vic_unmask_priority(s->daisy_callback);
        vic_update(s->daisy_callback);
    }
    vic_update(s);
}

void vic_init(void) {
    for (int i = 0; i < 2; i++) {
        vic_t *s = &vic[i];
        volatile u32 *w = (volatile u32 *)s;   /* no memset in this runtime */
        for (u32 k = 0; k < sizeof(*s) / 4; k++)
            w[k] = 0;
        s->sw_priority_mask = 0xFFFF;
        s->daisy_priority = 0xF;
        s->current = NO_IRQ;
        s->current_highest = NO_IRQ;
        s->priority = PRIO_LEVELS;
        s->priority_stack[0] = PRIO_LEVELS;
        s->irq_stack[0] = NO_IRQ;
        for (int j = 0; j < NSRC; j++)
            s->vect_priority[j] = 0xF;
    }
    vic[1].daisy = &vic[0];
}

u32 vic_read(int vic_index, u32 offset) {
    if (vic_index < 0 || vic_index > 1)
        return 0;
    vic_t *s = &vic[vic_index];

    if (offset >= 0x100 && offset < 0x180)
        return s->vect_addr[(offset - 0x100) >> 2];
    if (offset >= 0x200 && offset < 0x280)
        return s->vect_priority[(offset - 0x200) >> 2];

    switch (offset) {
    case VIC_IRQSTATUS:      return s->irq_status;
    case VIC_FIQSTATUS:      return s->fiq_status;
    case VIC_RAWINTR:        return s->rawintr | s->softint;
    case VIC_INTSELECT:      return s->intselect;
    case VIC_INTENABLE:      return s->intenable;
    case VIC_SOFTINT:        return s->softint;
    case VIC_PROTECTION:     return s->protection;
    case VIC_SWPRIORITYMASK: return s->sw_priority_mask;
    case VIC_PRIORITYDAISY:  return s->daisy_priority;
    case VIC_VECTADDR:       return vic_irq_ack(s);
    case 0xFE0: return 0x92;    /* PL192 peripheral ID */
    case 0xFE4: return 0x11;
    case 0xFE8: return 0x04;
    case 0xFEC: return 0x00;
    default:    return 0;
    }
}

void vic_write(int vic_index, u32 offset, u32 value) {
    if (vic_index < 0 || vic_index > 1)
        return;
    vic_t *s = &vic[vic_index];

    if (offset >= 0x100 && offset < 0x180) {
        s->vect_addr[(offset - 0x100) >> 2] = value;
        vic_update(s);
        return;
    }
    if (offset >= 0x200 && offset < 0x280) {
        s->vect_priority[(offset - 0x200) >> 2] = value & 0xF;
        vic_update(s);
        return;
    }

    switch (offset) {
    case VIC_INTSELECT:      s->intselect = value; break;
    case VIC_INTENABLE:      s->intenable |= value; break;
    case VIC_INTENCLEAR:     s->intenable &= ~value; break;
    case VIC_SOFTINT:        s->softint |= value; break;
    case VIC_SOFTINTCLEAR:   s->softint &= ~value; break;
    case VIC_PROTECTION:     s->protection = value & 1; break;
    case VIC_SWPRIORITYMASK: s->sw_priority_mask = value & 0xFFFF; break;
    case VIC_PRIORITYDAISY:  s->daisy_priority = value & 0xF; break;
    case VIC_VECTADDR:       vic_irq_fin(s); return;
    case PL190_VECTADDR:
    case PL190_DEFVECTADDR:
    case PL190_ITCR:
    default:
        return;
    }
    vic_update(s);
}

void vic_set_irq(int irq_num, bool level) {
    if (irq_num < 0 || irq_num >= 64)
        return;
    vic_t *s = &vic[irq_num >> 5];
    u32 bit = 1u << (irq_num & 31);
    u32 old = s->rawintr;
    if (level)
        s->rawintr |= bit;
    else
        s->rawintr &= ~bit;
    if (s->rawintr != old)
        vic_update(s);
}

bool vic_irq_pending(void) {
    return vic[0].irq_out;
}

bool vic_fiq_pending(void) {
    return vic[0].fiq_status || vic[1].fiq_status;
}

u32 vic_line_count(int line) {
    return (line >= 0 && line < 64) ? line_count[line] : 0;
}

/* VIC0 state for the status row:
 * acks, EOIs, current, priority mask, stack depth, enable, raw|soft. */
void vic_debug(u32 out[7]) {
    out[0] = n_ack;
    out[1] = n_eoi;
    out[2] = vic[0].current;
    out[3] = vic[0].priority;
    out[4] = vic[0].stack_i;
    out[5] = vic[0].intenable;
    out[6] = vic[0].rawintr | vic[0].softint;
}
