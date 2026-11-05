/*
 * S5L8900 I2C controller emulation, a port of qemu-ios
 * hw/i2c/ipod_touch_i2c.c (Samsung IIC block plus Apple's register 0x20,
 * whose 0x100 "byte done" and 0x2000 "stop done" bits clear on read).
 *
 * Quirks are kept on purpose: this exact behaviour is what lets the
 * iPhone OS I2C driver run in qemu-ios.
 */

#include "i2c.h"
#include "pmu.h"
#include "vic.h"

#define I2CCON      0x00
#define I2CSTAT     0x04
#define I2CADD      0x08
#define I2CDS       0x0C
#define I2CLC       0x10
#define IICREG20    0x20

#define IICCON_ACKEN        (1u << 7)
#define IICSTAT_START       (1u << 5)
#define IICSTAT_TXRXEN      (1u << 4)
#define IICSTAT_LASTBIT     (1u << 0)

#define REG20_BYTE_DONE     0x100
#define REG20_STOP_DONE     0x2000

enum { SR_MODE, ST_MODE, MR_MODE, MT_MODE };

typedef struct {
    u8   control, status, address, data, line_ctrl;
    u32  reg20;
    bool active;
    bool slave_present;     /* the addressed device exists */
} i2c_bus_t;

static i2c_bus_t buses[2];

/* Traffic counters for the status row (bytes clocked, IRQs raised). */
static u32 n_start[2], n_tx[2], n_rx[2], n_irq[2];

/* Interrupt lines (device tree i2c0/i2c1 "interrupts"); qemu-ios raises
 * the line after each byte and control write and drops it on any write. */
static void bus_irq(int bus, bool level)
{
    if (level)
        n_irq[bus]++;
    vic_set_irq(bus ? VIC_LINE_I2C1 : VIC_LINE_I2C0, level);
}

static bool slave_start(int bus, u8 addr)
{
    return bus == 1 && addr == PMU_I2C_ADDR;
}

static void bus_send(i2c_bus_t *b, u8 byte)
{
    if (b->active && b->slave_present)
        pmu_i2c_send(byte);
}

static u8 bus_recv(i2c_bus_t *b)
{
    if (b->active && b->slave_present)
        return pmu_i2c_recv();
    return 0xFF;
}

static void start_transfer(int bus, i2c_bus_t *b)
{
    n_start[bus]++;
    b->status &= ~IICSTAT_LASTBIT;
    b->reg20 |= REG20_BYTE_DONE;
    b->active = true;
    b->slave_present = slave_start(bus, b->data >> 1);
    if (b->slave_present)
        pmu_i2c_start();
}

static void end_transfer(i2c_bus_t *b)
{
    b->active = false;
    b->slave_present = false;
}

void i2c_init(void)
{
    for (int i = 0; i < 2; i++)
        buses[i] = (i2c_bus_t){ 0 };
}

u32 i2c_read(int bus, u32 offset)
{
    if (bus < 0 || bus > 1)
        return 0;
    i2c_bus_t *b = &buses[bus];

    switch (offset) {
    case I2CCON:
        return b->control;
    case I2CSTAT:
        return b->status;
    case I2CADD:
        return b->address;
    case I2CDS:
        b->reg20 |= REG20_BYTE_DONE;
        b->data = bus_recv(b);
        n_rx[bus]++;
        bus_irq(bus, true);
        return b->data;
    case I2CLC:
        return b->line_ctrl;
    case IICREG20: {
        u32 v = b->reg20;
        b->reg20 &= ~(REG20_BYTE_DONE | REG20_STOP_DONE);
        return v;
    }
    default:
        return 0;
    }
}

void i2c_write(int bus, u32 offset, u32 value)
{
    if (bus < 0 || bus > 1)
        return;
    i2c_bus_t *b = &buses[bus];

    bus_irq(bus, false);
    switch (offset) {
    case I2CCON:
        if (value & ~IICCON_ACKEN)
            b->reg20 |= REG20_BYTE_DONE;
        if ((value & 0x10) && b->status == 0x90)
            b->reg20 |= REG20_STOP_DONE;
        b->control = (u8)value;
        bus_irq(bus, true);
        break;

    case I2CSTAT: {
        u32 old_mode = b->status >> 6, new_mode = (value >> 6) & 3;
        if (!b->active && old_mode != new_mode) {
            b->status = (u8)value;
        } else if (b->active && old_mode != new_mode) {
            /* Mode toggled mid-transfer: end it before switching. */
            end_transfer(b);
            b->status = (u8)value | IICSTAT_TXRXEN;
            break;
        }
        if (value & IICSTAT_TXRXEN) {
            switch ((b->status >> 6) & 3) {
            case SR_MODE:
            case ST_MODE:
                b->data = bus_recv(b);
                break;
            case MR_MODE:
            case MT_MODE:
                if (value & IICSTAT_START) {
                    start_transfer(bus, b);
                } else {
                    end_transfer(b);
                    b->status |= IICSTAT_TXRXEN;
                }
                break;
            }
        }
        /* qemu-ios ends every such STAT write with s5l8900_i2c_update(),
         * which raises the line: the interrupt after the address byte (or
         * a stop) that the driver sleeps for.  The earlier writes of the
         * sequence raise it too, but each write drops it first and the
         * driver programs them with interrupts off, so only this last raise
         * survives to be taken. */
        bus_irq(bus, true);
        break;
    }

    case I2CADD:
        b->address = (u8)value;
        break;

    case I2CDS:
        if (!(b->status & IICSTAT_LASTBIT)) {
            b->status |= IICCON_ACKEN;  /* sic: qemu-ios sets this in STAT */
            b->data = (u8)value;
            b->reg20 |= REG20_BYTE_DONE;
            n_tx[bus]++;
            bus_send(b, b->data);
        }
        bus_irq(bus, true);
        break;

    case I2CLC:
        b->line_ctrl = (u8)value;
        break;
    }
}

extern u32 pmu_reads, pmu_last_reg;

/* "I1 s<starts> t<bytes out> r<bytes in> q<irqs raised> a<irqs acked>
 *  reg<last PMU register read> I0 s t" */
void i2c_status_line(char *buf)
{
    static const char h[] = "0123456789ABCDEF";
    char *p = buf;
#define FIELD(tag, v, dg) do { *p++ = (tag); \
        for (int i_ = (dg) - 1; i_ >= 0; i_--) *p++ = h[((v) >> (i_ * 4)) & 0xF]; } while (0)
    *p++ = 'I'; *p++ = '1';
    *p++ = ' '; FIELD('s', n_start[1], 3);
    *p++ = ' '; FIELD('t', n_tx[1], 3);
    *p++ = ' '; FIELD('r', n_rx[1], 3);
    *p++ = ' '; FIELD('q', n_irq[1], 4);
    *p++ = ' '; FIELD('a', vic_line_count(VIC_LINE_I2C1), 3);
    *p++ = ' '; FIELD('R', pmu_last_reg, 2);
    *p++ = ' '; *p++ = '0';
    *p++ = ' '; FIELD('s', n_start[0], 2);
    *p++ = ' '; FIELD('a', vic_line_count(VIC_LINE_I2C0), 2);
#undef FIELD
    *p = 0;
}
