/*
 * Input: 3DS touchscreen and pad -> the iPod touch's Zephyr multitouch
 * controller (SPI2) and Home/Hold buttons (GPIO).
 *
 * 3DS side (ARM11, polled from the heartbeat):
 *   - touchscreen through the CODEC on the new SPI interface, bus
 *     0x10142800 CS0, exactly as GodMode9's arm11 hw/codec.c + common/spi.c:
 *     init sequence once, then page 0xFB register 1, 0x34 bytes; pressed
 *     when byte 0 bit 4 is clear, x = bytes 0/1, y = bytes 10/11 (12 bit).
 *   - pad at 0x10146000 (active low): START -> Home, SELECT -> Hold.
 *
 * iPod side, after qemu-ios t1:
 *   - multitouch: hw/arm/ipod_touch_multitouch.c command/response state
 *     machine on SPI2 and its single-finger "path" frames; a new frame is
 *     announced with GPIO interrupt 0x9B (group 4 bit 27).
 *   - buttons: GPIO +0x2C4 bit 5 (hold, pin 0x1605) and bit 6 (menu, pin
 *     0x1606), set while pressed; each change raises GPIO interrupt 0x2D /
 *     0x2E (hw/arm/ipod_touch.c ipod_touch_key_event).
 */
#include "input.h"
#include "sysic.h"
#include "pmu.h"
#include "../iowin.h"
#if PORTRAIT
#include "../portrait.h"
#endif
#ifndef LCD_H
#define LCD_H 240
#endif

/* ── 3DS new-SPI CODEC access ───────────────────────────────────────── */

#define SPI_BUS1        0x10142800u
#define SPI_CONTROL     0x00
#define SPI_DONE        0x04
#define SPI_BLKLEN      0x08
#define SPI_FIFO        0x0C
#define SPI_STAT        0x10
#define SPI_CODEC_REG   5u                  /* rate 5, chip select 0 */
#define SPI_WRITE       (1u << 13)
#define SPI_START       (1u << 15)
#define REG_CFG_SPI_CNT (*(volatile u16 *)io_va(0x101401C0u))
#define REG_HID_PAD     (*(volatile u16 *)io_va(0x10146000u))

#define SPIR(r)         IOREG32(SPI_BUS1 + (r))

static bool spi_wait(u32 reg, u32 bit) {
    for (u32 i = 0; i < 200000; i++)
        if (!(SPIR(reg) & bit))
            return true;
    return false;
}

static bool spi_single(void *buffer, u32 len, bool read) {
    u32 *w = buffer;
    SPIR(SPI_BLKLEN) = len;
    SPIR(SPI_CONTROL) = SPI_CODEC_REG | (read ? 0 : SPI_WRITE) | SPI_START;
    if (!spi_wait(SPI_STAT, 1))
        return false;
    for (u32 pos = 0; pos < len; pos += 4) {
        if ((pos % 32) == 0 && !spi_wait(SPI_STAT, 1))
            return false;
        if (read)
            w[pos / 4] = SPIR(SPI_FIFO);
        else
            SPIR(SPI_FIFO) = w[pos / 4];
    }
    return true;
}

/* One chip-select cycle: optional write then optional read. */
static bool spi_xfer(void *tx, u32 txl, void *rx, u32 rxl) {
    bool ok = true;
    if (tx && txl)
        ok = spi_wait(SPI_CONTROL, SPI_START) && spi_single(tx, txl, false);
    if (ok && rx && rxl)
        ok = spi_wait(SPI_CONTROL, SPI_START) && spi_single(rx, rxl, true);
    ok = spi_wait(SPI_CONTROL, SPI_START) && ok;
    SPIR(SPI_DONE) = 0;
    return ok;
}

static void codec_select(u8 page) {
    u32 cmd = (u32)page << 8;
    spi_xfer(&cmd, 2, 0, 0);
}

static u8 codec_read(u8 reg) {
    u32 cmd = ((u32)reg << 1) | 1, ret = 0;
    spi_xfer(&cmd, 1, &ret, 1);
    return (u8)ret;
}

static void codec_write(u8 reg, u8 val) {
    u32 cmd = ((u32)val << 8) | ((u32)reg << 1);
    spi_xfer(&cmd, 2, 0, 0);
}

static void codec_mask(u8 reg, u8 set, u8 mask) {
    codec_write(reg, (u8)((codec_read(reg) & ~mask) | (set & mask)));
}

static bool codec_ok;

void input_init(void) {
    REG_CFG_SPI_CNT = 7;                    /* new SPI interface on all buses */
    codec_select(0x67);
    codec_write(0x24, 0x98);
    codec_write(0x26, 0x00);
    codec_write(0x25, 0x43);
    codec_write(0x24, 0x18);
    codec_write(0x17, 0x43);
    codec_write(0x19, 0x69);
    codec_write(0x1B, 0x80);
    codec_write(0x27, 0x11);
    codec_write(0x26, 0xEC);
    codec_write(0x24, 0x18);
    codec_write(0x25, 0x53);
    codec_mask(0x26, 0x80, 0x80);
    codec_mask(0x24, 0x00, 0x80);
    codec_mask(0x25, 0x10, 0x3C);
    codec_ok = true;
}

/* Raw 12-bit touch sample; false when not pressed. */
static bool touch_read(u32 *rx, u32 *ry) {
    u32 buf[0x34 / 4];
    u8 *b = (u8 *)buf;
    u32 cmd = (1u << 1) | 1;
    codec_select(0xFB);
    if (!spi_xfer(&cmd, 1, buf, 0x34))
        return false;
    if (b[0] & 0x10)
        return false;
    *rx = ((u32)b[0] << 8 | b[1]) & 0xFFF;
    *ry = ((u32)b[10] << 8 | b[11]) & 0xFFF;
    return true;
}

/* ── Zephyr multitouch on SPI2 ──────────────────────────────────────────
 *
 * After qemu-ios ipod_touch_multitouch.c, corrected against the iOS 1.1.5
 * AppleMultitouchZ2SPI driver (prelinked kext, __text at 0xC0442000):
 *
 *  - Every SPI transaction is a separate chip-select cycle and the
 *    controller parses each from its first byte (HBPP data excepted, see
 *    mt_cs); AppleS5L8900XSPIController writes CTRL exactly once per
 *    transaction (c0494954), so mt_cs() is called there.  Several commands may share one transaction (ATN_ACK is
 *    1A A1 18 E1 18 E1 18 E1; firmware/calibration is 18 E1 30 ...).
 *  - Frame read (readOneFrameOfData c044ad50) is two transactions:
 *      stage 1, 16 bytes  tx {C, seq, 0, 0.., ck16}  rx {C, len lo, len hi, 0.., ck16}
 *      stage 2, len+5     tx {C, seq, 1, ...}         rx {C, x, len lo, len hi, pad, data, ck}
 *    with the first five rx bytes of stage 2 summing to 0.  C is 0xEA, or
 *    0xEB once the cached bcdVersion is > 0x23 (c044c148 sets +0x1BC;
 *    c044a9cc picks the byte): with our bcdVersion 51 the driver uses
 *    0xEB, which the old model did not know - it echoed one byte and
 *    parsed the rest of the packet as new commands until a stray 0x30
 *    started a phantom HBPP packet that swallowed every later byte.
 *  - Long control write/read 0xE5/0xE7 (c044d344, c044ca28): 16-byte
 *    stage 1 with the length at [3..4], then a len+5 byte stage 2.
 */

#define MT_INTERFACE_VERSION     0x1
#define MT_FAMILY_ID             81
#define MT_ENDIANNESS            0x1
#define MT_SENSOR_ROWS           15
#define MT_SENSOR_COLUMNS        10
#define MT_BCD_VERSION           51
#define MT_MAX_PACKET_SIZE       0x294
#define MT_SENSOR_SURFACE_WIDTH  5000
#define MT_SENSOR_SURFACE_HEIGHT 7500
#define MT_INT_W   ((9000 - MT_SENSOR_SURFACE_WIDTH) * 84 / 73)
#define MT_INT_H   ((13850 - MT_SENSOR_SURFACE_HEIGHT) * 84 / 73)

#define MT_REPORT_UNKNOWN1            0x70
#define MT_REPORT_FAMILY_ID           0xD1
#define MT_REPORT_SENSOR_INFO         0xD3
#define MT_REPORT_SENSOR_REGION_DESC  0xD0
#define MT_REPORT_SENSOR_REGION_PARAM 0xA1
#define MT_REPORT_SENSOR_DIMENSIONS   0xD9

#define MT_CMD_HBPP_DATA_PACKET      0x30
#define MT_CMD_GET_CMD_STATUS        0xE1
#define MT_CMD_GET_INTERFACE_VERSION 0xE2
#define MT_CMD_GET_REPORT_INFO       0xE3
#define MT_CMD_SHORT_CONTROL_WRITE   0xE4
#define MT_CMD_LONG_CONTROL_WRITE    0xE5
#define MT_CMD_SHORT_CONTROL_READ    0xE6
#define MT_CMD_LONG_CONTROL_READ     0xE7
#define MT_CMD_FRAME_READ            0xEA
#define MT_CMD_FRAME_READ_SEQ        0xEB

#define MT_FRAME_TYPE_PATH      0x44
#define MT_EVENT_TOUCH_FULL_END 0x0
#define MT_EVENT_TOUCH_START    0x3
#define MT_EVENT_TOUCH_MOVED    0x4
#define MT_EVENT_TOUCH_ENDED    0x7

#define GPIO_IRQ_MULTITOUCH     0x9B
#define GPIO_IRQ_HOLD           0x2D
#define GPIO_IRQ_MENU           0x2E

#define MT_REST                 0xFFFFFFFFu     /* until chip select ends */

typedef struct {
    u8  type, frameNum, headerLen, unk_3;
    u32 timestamp;
    u8  unk_8, unk_9, unk_A, unk_B;
    u16 unk_C, isImage;
    u8  numFingers, fingerDataLen;
    u16 unk_12, unk_14, unk_16;
} __attribute__((packed)) mt_hdr_t;

typedef struct {
    u8  id, event, unk_2, unk_3;
    s16 x, y, velX, velY;
    u16 radius2, radius3, angle, radius1, contactDensity;
    u16 unk_16, unk_18, unk_1A;
} __attribute__((packed)) mt_finger_t;

/* A queued frame's data: header, one finger, 16-bit checksum (54 bytes). */
typedef struct {
    mt_hdr_t    header;
    mt_finger_t finger;
    u8 checksum1, checksum2;
} __attribute__((packed)) mt_payload_t;

#define MT_PAYLOAD  sizeof(mt_payload_t)
#define MT_QLEN     8

static mt_payload_t mt_q[MT_QLEN];
static u32 mt_q_head, mt_q_n;

static struct {
    u8  cur_cmd;
    u32 buf_size, buf_ind, in_ind;
    u8  in[16];
    u8  out[64];
    u8  ack[2];
    u8  last_cmd;
    /* frame read */
    bool fr_stage2;
    u8   fr_len[16];
    u8   fr_data[MT_PAYLOAD + 5];
    u32  fr_n;                  /* stage 2 length, 0 with no frame */
    u16  long_len;              /* 0xE7 length from its stage 1 */
    /* diagnostics */
    u32 cmds, len_reads, frames_read, unknown, cs_count, cs_kept;
} mt;

static struct {
    u32 raises;                 /* GPIO 0x9B raises (new frame or re-raise) */
    u32 last_raise_ms;
    u32 dropped;
} mtirq;

static u16 sum16(const u8 *p, u32 n) {
    u32 c = 0;
    for (u32 i = 0; i < n; i++)
        c += p[i];
    return (u16)c;
}

static void mt_checksum16(void) {
    u16 c = sum16(mt.out, 14);
    mt.out[14] = (u8)c;
    mt.out[15] = (u8)(c >> 8);
}

static void mt_zero_out(u32 from) {
    for (u32 i = from; i < sizeof(mt.out); i++)
        mt.out[i] = 0;
}

static u32 mt_report_len(u8 id) {
    static const struct { u8 id, len; } sizes[] = {
        { MT_REPORT_UNKNOWN1, 1 }, { MT_REPORT_FAMILY_ID, 1 },
        { MT_REPORT_SENSOR_INFO, 5 }, { MT_REPORT_SENSOR_REGION_DESC, 1 },
        { MT_REPORT_SENSOR_REGION_PARAM, 1 }, { MT_REPORT_SENSOR_DIMENSIONS, 8 },
    };
    for (u32 i = 0; i < sizeof(sizes) / sizeof(sizes[0]); i++)
        if (sizes[i].id == id)
            return sizes[i].len;
    return 0;
}

static void mt_report_info(u8 id) {
    mt_zero_out(1);
    mt.out[3] = (u8)mt_report_len(id);
    mt_checksum16();
}

/* Report contents at d (rx[3..] of a control read). */
static void mt_report_data(u8 id, u8 *d) {
    switch (id) {
    case MT_REPORT_FAMILY_ID:
        d[0] = MT_FAMILY_ID;
        break;
    case MT_REPORT_SENSOR_INFO:
        d[0] = MT_ENDIANNESS;
        d[1] = MT_SENSOR_ROWS;
        d[2] = MT_SENSOR_COLUMNS;
        d[3] = MT_BCD_VERSION & 0xFF;
        d[4] = MT_BCD_VERSION >> 8;
        break;
    case MT_REPORT_SENSOR_DIMENSIONS:
        d[0] = MT_SENSOR_SURFACE_WIDTH & 0xFF;
        d[1] = (MT_SENSOR_SURFACE_WIDTH >> 8) & 0xFF;
        d[4] = MT_SENSOR_SURFACE_HEIGHT & 0xFF;
        d[5] = (MT_SENSOR_SURFACE_HEIGHT >> 8) & 0xFF;
        break;
    default:
        break;                  /* region desc/param and unknown: 0 */
    }
}

static void mt_short_control(u8 id) {
    mt_zero_out(1);
    mt_report_data(id, &mt.out[3]);
    mt_checksum16();
}

/* Both stages' answers for a frame read with command byte c, from the
 * oldest queued frame (or "length 0" with none).  Byte 1 is the same in
 * both, since the stage is only known when byte 2 arrives. */
static void mt_frame_prepare(u8 c) {
    u16 len = mt_q_n ? (u16)MT_PAYLOAD : 0;
    for (u32 i = 0; i < sizeof(mt.fr_len); i++)
        mt.fr_len[i] = 0;
    mt.fr_len[0] = c;
    mt.fr_len[1] = (u8)len;
    mt.fr_len[2] = (u8)(len >> 8);
    u16 ck = sum16(mt.fr_len, 14);
    mt.fr_len[14] = (u8)ck;
    mt.fr_len[15] = (u8)(ck >> 8);

    mt.fr_n = 0;
    if (!len)
        return;
    u8 *d = mt.fr_data;
    d[0] = c;
    d[1] = (u8)len;
    d[2] = (u8)len;
    d[3] = (u8)(len >> 8);
    d[4] = (u8)(0x100 - (sum16(d, 4) & 0xFF));
    const u8 *src = (const u8 *)&mt_q[mt_q_head];
    for (u32 i = 0; i < MT_PAYLOAD; i++)
        d[5 + i] = src[i];
    mt.fr_n = 5 + MT_PAYLOAD;
}

/* A new chip-select cycle: the controller starts parsing afresh - except
 * inside an HBPP data packet, whose payload may span several SPI
 * transactions (the 0xBFE8-byte "Constructed Firmware" arrives in ~4 KB
 * pieces; a 0xFDD-byte one was seen on hardware).  Resetting there made
 * the model parse firmware bytes as commands (a stray 0x1E changes the
 * ATN_ACK answer, a stray 0x30 swallows traffic), which regressed boot. */
void mt_cs(void) {
    mt.cs_count++;
    if (mt.cur_cmd == MT_CMD_HBPP_DATA_PACKET && mt.buf_ind < mt.buf_size) {
        mt.cs_kept++;
        return;
    }
    mt.cur_cmd = 0;
    mt.buf_size = 0;
}

/* One byte clocked through the controller (SPI2, full duplex). */
u8 mt_transfer(u8 value) {
    if (mt.cur_cmd == 0) {
        mt.cmds++;
        mt.cur_cmd = value;
        mt.last_cmd = value;
        mt.buf_ind = 0;
        mt.in_ind = 0;
        mt_zero_out(0);
        mt.out[0] = value;
        switch (value) {
        case 0x18:                      /* filler */
            mt.buf_size = 2;
            mt.out[1] = 0xE1;
            break;
        case 0x1A:                      /* HBPP ack */
            mt.buf_size = 2;
            if (!mt.ack[0] && !mt.ack[1]) {
                mt.out[0] = 0x4B;
                mt.out[1] = 0xC1;
            } else {
                mt.out[0] = mt.ack[0];
                mt.out[1] = mt.ack[1];
            }
            break;
        case 0x1C: mt.buf_size = 8;  mt.out[0] = 0; break;  /* read register */
        case 0x1D: mt.buf_size = 12; mt.out[0] = 0; break;  /* execute */
        case 0x1E: mt.buf_size = 16; mt.out[0] = 0; break;  /* write register */
        case 0x1F: mt.buf_size = 2; break;                  /* calibration */
        case MT_CMD_HBPP_DATA_PACKET: mt.buf_size = 20; break;
        case 0x47: mt.buf_size = 2; break;                  /* clear interrupt */
        case MT_CMD_GET_CMD_STATUS:
            mt.buf_size = 16;
            mt_zero_out(1);
            mt_checksum16();
            break;
        case MT_CMD_GET_INTERFACE_VERSION:
            mt.buf_size = 16;
            mt_zero_out(1);
            mt.out[2] = MT_INTERFACE_VERSION;
            mt.out[3] = MT_MAX_PACKET_SIZE & 0xFF;
            mt.out[4] = MT_MAX_PACKET_SIZE >> 8;
            mt_checksum16();
            break;
        case MT_CMD_GET_REPORT_INFO:
        case MT_CMD_SHORT_CONTROL_WRITE:
        case MT_CMD_SHORT_CONTROL_READ:     /* both stages are 16 bytes */
            mt.buf_size = 16;
            break;
        case MT_CMD_LONG_CONTROL_WRITE:     /* stage 1, then len+5: accept */
            mt.buf_size = MT_REST;
            mt.out[0] = 0;
            break;
        case MT_CMD_LONG_CONTROL_READ:      /* sized once byte 2 is known */
            mt.buf_size = 16;
            break;
        case MT_CMD_FRAME_READ:
        case MT_CMD_FRAME_READ_SEQ:
            mt.buf_size = 16;               /* stage 2 set at byte 2 */
            mt.fr_stage2 = false;
            mt_frame_prepare(value);
            break;
        default:                        /* unknown: answer 0 to the end */
            mt.buf_size = MT_REST;
            mt.unknown++;
            break;
        }
    }

    if (mt.in_ind < sizeof(mt.in))
        mt.in[mt.in_ind] = value;
    mt.in_ind++;
    bool frame = mt.cur_cmd == MT_CMD_FRAME_READ || mt.cur_cmd == MT_CMD_FRAME_READ_SEQ;

    if (mt.cur_cmd == MT_CMD_HBPP_DATA_PACKET && mt.in_ind == 10) {
        /* header carries the payload length (c044ee98); answer with 0s */
        mt.buf_size = (((u32)mt.in[2] << 10) | ((u32)mt.in[3] << 2)) + 5;
        mt.buf_ind = 0;
        mt_zero_out(0);
    } else if (mt.cur_cmd == MT_CMD_GET_REPORT_INFO && mt.in_ind == 2) {
        mt_report_info(mt.in[1]);
    } else if (mt.cur_cmd == MT_CMD_SHORT_CONTROL_READ && mt.in_ind == 2) {
        mt_short_control(mt.in[1]);
    } else if (mt.cur_cmd == MT_CMD_LONG_CONTROL_READ && mt.in_ind == 3 && value) {
        /* stage 2: {E7, 0, 0, data[len], ck16 over len+3 bytes} */
        u32 len = mt.long_len;
        if (len > sizeof(mt.out) - 5)
            len = sizeof(mt.out) - 5;
        mt_zero_out(1);
        mt_report_data(mt.in[1], &mt.out[3]);
        u16 ck = sum16(mt.out, len + 3);
        mt.out[len + 3] = (u8)ck;
        mt.out[len + 4] = (u8)(ck >> 8);
        mt.buf_size = (u32)mt.long_len + 5;
    } else if (mt.cur_cmd == MT_CMD_LONG_CONTROL_READ && mt.in_ind == 5 && !mt.in[2]) {
        mt.long_len = (u16)(mt.in[3] | mt.in[4] << 8);   /* stage 1 */
    } else if (frame && mt.in_ind == 3) {
        mt.fr_stage2 = value != 0;
        if (mt.fr_stage2)
            mt.buf_size = mt.fr_n ? mt.fr_n : MT_REST;
        else
            mt.len_reads++;
    }

    u8 ret;
    if (frame) {
        u32 i = mt.buf_ind;
        if (mt.fr_stage2)
            ret = i < mt.fr_n ? mt.fr_data[i] : 0;
        else
            ret = i < sizeof(mt.fr_len) ? mt.fr_len[i] : 0;
    } else {
        ret = mt.buf_ind < sizeof(mt.out) ? mt.out[mt.buf_ind] : 0;
    }
    mt.buf_ind++;

    if (mt.buf_ind >= mt.buf_size) {
        /* The next ATN_ACK (0x1A) answers the last operation: the driver
         * wants 4AD1 after a register write (c044bd68) and 4BC1 after an
         * HBPP data packet (firmware c044e05c, calibration c044e698).  A
         * sticky 4AD1 failed every firmware re-upload after the first
         * boot's calibration writes: on Home/wake the driver re-bootloads,
         * fails 3 times, finds the chip still in HBPP and kills it
         * ("No firmware running, and couldn't load any"). */
        if (mt.cur_cmd == 0x1E) {
            mt.ack[0] = 0x4A;
            mt.ack[1] = 0xD1;
        } else if (mt.cur_cmd == MT_CMD_HBPP_DATA_PACKET) {
            mt.ack[0] = 0x4B;
            mt.ack[1] = 0xC1;
        }
        if (frame && mt.fr_stage2 && mt.fr_n && mt_q_n) {
            /* frame delivered: drop it (the next one is announced by
             * mt_tick if the interrupt is not still pending) */
            mt_q_head = (mt_q_head + 1) % MT_QLEN;
            mt_q_n--;
            mt.frames_read++;
        }
        mt.cur_cmd = 0;
        mt.buf_size = 0;
    }
    return ret;
}

static u32 mt_frame_counter, mt_last_ms;
static s32 mt_prev_x, mt_prev_y;

static void mt_raise(u32 now_ms) {
    mtirq.raises++;
    mtirq.last_raise_ms = now_ms;
    sysic_gpio_raise(GPIO_IRQ_MULTITOUCH);
}

/* Queue the next single-finger frame; x/y are 0..65535 fractions of the
 * display from the top-left, mapped like qemu-ios's mouse handler (sensor
 * y runs bottom-up). */
static void mt_make_frame(u8 event, u32 fx, u32 fy, u32 now_ms, bool down) {
    mt_payload_t *f;
    if (mt_q_n == MT_QLEN) {
        /* full: drop a new drag frame; otherwise replace the newest */
        mtirq.dropped++;
        if (event == MT_EVENT_TOUCH_MOVED)
            return;
        f = &mt_q[(mt_q_head + mt_q_n - 1) % MT_QLEN];
    } else {
        f = &mt_q[(mt_q_head + mt_q_n) % MT_QLEN];
        mt_q_n++;
    }
    u8 *p = (u8 *)f;
    for (u32 i = 0; i < sizeof(*f); i++)
        p[i] = 0;

    mt_hdr_t *h = &f->header;
    h->type = MT_FRAME_TYPE_PATH;
    h->frameNum = (u8)mt_frame_counter;
    h->headerLen = sizeof(mt_hdr_t);
    h->timestamp = now_ms;
    h->numFingers = 1;
    h->fingerDataLen = sizeof(mt_finger_t);

    s32 x = (s32)(fx * MT_INT_W / 65536u);
    s32 y = (s32)((65536u - fy) * MT_INT_H / 65536u);
    s32 dt = (s32)(now_ms - mt_last_ms) + 1;
    mt_finger_t *g = &f->finger;
    g->id = 1;
    g->event = event;
    g->unk_2 = 2;
    g->unk_3 = 1;
    g->x = (s16)x;
    g->y = (s16)y;
    g->velX = (s16)((x - mt_prev_x) * 1000 / dt);
    g->velY = (s16)((y - mt_prev_y) * 1000 / dt);
    if (down) {
        g->radius1 = 100;
        g->radius2 = 660;
        g->radius3 = 580;
        g->contactDensity = 150;
    }
    g->angle = 19317;

    u16 c = sum16((const u8 *)f, MT_PAYLOAD - 2);
    f->checksum1 = (u8)c;
    f->checksum2 = (u8)(c >> 8);

    mt_prev_x = x;
    mt_prev_y = y;
    mt_last_ms = now_ms;
    mt_frame_counter++;
    mt_raise(now_ms);
}

/* ATN stays asserted on the real part while frames are waiting: announce
 * again when a frame is queued but the GPIO interrupt is no longer
 * latched (the kernel acked it before the read, or a raise was lost). */
void mt_tick(u32 now_ms) {
    if (mt_q_n && !sysic_gpio_pending(GPIO_IRQ_MULTITOUCH) &&
        now_ms - mtirq.last_raise_ms >= 40)
        mt_raise(now_ms);
}

/* ── Buttons (GPIO port 0x16 data, +0x2C4) ──────────────────────────── */

static u32 button_bits;         /* bit 5 hold, bit 6 menu: set while pressed */

u32 input_gpio_read(u32 offset) {
    return offset == 0x2C4 ? button_bits : 0;
}

/* ── Heartbeat poll ─────────────────────────────────────────────────── */

static struct {
    u32 pad;
    u32 raw_x, raw_y, x, y;     /* x, y: panel pixels (320 x LCD_H) */
    u32 fx, fy;                 /* last contact, 0..65535 of the panel */
    bool down;
    u32 events;
} in;

#define PAD_A       (1u << 0)
#define PAD_SELECT  (1u << 2)
#define PAD_START   (1u << 3)

/* Raw 12-bit CODEC sample -> panel position as 0..65535 fractions.  The
 * raw range maps linearly (uncalibrated) onto the 320x240 bottom screen;
 * in portrait builds that point goes back through the view rotation and
 * scale (portrait.c), otherwise the panel is the screen stretched. */
static void touch_map(u32 rx, u32 ry, u32 *fx, u32 *fy) {
#if PORTRAIT
    pt_touch(rx * 320 / 4096, ry * 240 / 4096, fx, fy);
#else
    *fx = rx * 16;
    *fy = ry * 16;
#endif
}

/* One touch sample (raw 12-bit CODEC values) -> multitouch frames:
 * START on contact, MOVED at 20 Hz while held, ENDED on release and
 * FULL_END 100 ms later (qemu-ios's sequence). */
void input_touch_sample(bool down, u32 rx, u32 ry, u32 now_ms) {
    static u32 moved_at, ended_at;
    if (down) {
        in.raw_x = rx;
        in.raw_y = ry;
        u32 fx, fy;
        touch_map(rx, ry, &fx, &fy);
        in.fx = fx;
        in.fy = fy;
        in.x = fx * 320 >> 16;
        in.y = fy * LCD_H >> 16;
        if (!in.down) {
            mt_make_frame(MT_EVENT_TOUCH_START, fx, fy, now_ms, true);
            moved_at = now_ms;
            in.events++;
        } else if (now_ms - moved_at >= 50) {   /* 20 Hz drag frames */
            mt_make_frame(MT_EVENT_TOUCH_MOVED, fx, fy, now_ms, true);
            moved_at = now_ms;
        }
        ended_at = 0;
    } else if (in.down) {
        mt_make_frame(MT_EVENT_TOUCH_ENDED, in.fx, in.fy, now_ms, false);
        ended_at = now_ms;
        in.events++;
    } else if (ended_at && now_ms - ended_at >= 100) {
        mt_make_frame(MT_EVENT_TOUCH_FULL_END, in.fx, in.fy, now_ms, false);
        ended_at = 0;
    }
    in.down = down;
}

/* SELECT = Hold, START = Home (and A, under the right thumb in the
 * portrait book grip) */
#if PORTRAIT
#define PAD_HOME    (PAD_START | PAD_A)
#else
#define PAD_HOME    PAD_START
#endif

/* 3DS pad, pressed bits. */
u32 input_pad_now(void) {
    return ~(u32)REG_HID_PAD & 0xFFF;
}

/* The iPod buttons in pad that wake it from sleep (Hold, Home). */
u32 input_wake_buttons(u32 pad) {
    return pad & (PAD_SELECT | PAD_HOME);
}

/* Pad -> iPod button state (+0x2C4) and a GPIO interrupt per change. */
static void buttons_update(u32 pad) {
    u32 nb = ((pad & PAD_SELECT) ? (1u << 5) : 0) | ((pad & PAD_HOME) ? (1u << 6) : 0);
    u32 changed = nb ^ button_bits;
    button_bits = nb;
    if (changed & (1u << 5))
        sysic_gpio_raise(GPIO_IRQ_HOLD);
    if (changed & (1u << 6))
        sysic_gpio_raise(GPIO_IRQ_MENU);
    /* both buttons also reach the PMU's EXTON1 input, the wake source
     * that still works while the button driver is powered down */
    if (changed & nb)
        pmu_button_event(true);
    else if (changed)
        pmu_button_event(false);
    in.pad = pad;
}

/* Woken from sleep by pad (pm.c): latch the press as the iPod button
 * that woke it; the release follows from input_poll. */
void input_wake(u32 pad) {
    buttons_update(pad);
}

/* Called on every 100 Hz heartbeat. */
void input_poll(u32 beat) {
    if (beat & 1)                       /* 50 Hz */
        return;
    u32 now_ms = beat * 10;

    buttons_update(input_pad_now());

    if (codec_ok) {
        u32 rx = 0, ry = 0;
        bool down = touch_read(&rx, &ry);
        input_touch_sample(down, rx, ry, now_ms);
    }
    mt_tick(now_ms);
}

static char *hx(char *p, u32 v, u32 digits) {
    static const char h[] = "0123456789ABCDEF";
    for (int i = (int)digits - 1; i >= 0; i--)
        *p++ = h[(v >> (i * 4)) & 0xF];
    return p;
}

/* "T<x>,<y> <D|u> r<raw x>,<raw y> B<buttons> e<touch events>
 *  (x, y in panel pixels)
 *  f<frames delivered>/<length reads> i<IRQ raises>/<kernel acks>
 *  E<IRQ enabled> q<frames queued>"  (50 columns at 480 lines; "!"
 *  instead of q when the CODEC did not initialise) */
void input_status_line(char *buf) {
    u32 st = sysic_gpio_state(GPIO_IRQ_MULTITOUCH);
    char *p = buf;
    *p++ = 'T';
    p = hx(p, in.x, 3); *p++ = ',';
    p = hx(p, in.y, LCD_H > 256 ? 3 : 2); *p++ = ' ';
    *p++ = in.down ? 'D' : 'u';
    *p++ = ' '; *p++ = 'r';
    p = hx(p, in.raw_x, 3); *p++ = ',';
    p = hx(p, in.raw_y, 3);
    *p++ = ' '; *p++ = 'B'; p = hx(p, button_bits >> 5, 1);
    *p++ = ' '; *p++ = 'e'; p = hx(p, in.events, 2);
    *p++ = ' '; *p++ = 'f'; p = hx(p, mt.frames_read, 3);
    *p++ = '/'; p = hx(p, mt.len_reads, 3);
    *p++ = ' '; *p++ = 'i'; p = hx(p, mtirq.raises, 3);
    *p++ = '/'; p = hx(p, st >> 8, 3);
    *p++ = ' '; *p++ = 'E'; *p++ = (st & 2) ? '1' : '0';
    *p++ = ' '; *p++ = codec_ok ? 'q' : '!'; p = hx(p, mt_q_n, 1);
    *p = 0;
}

/* "BTN h<raises>/<acks><bits> m<...> k<state> PMU <raises>/<INT reads>
 *  x<INT2 pending><INT2 mask>": Hold (GPIO 0x2D) and Menu (0x2E)
 * interrupts; bits = 1 enabled | 2 type | 4 level as the kernel set them;
 * k = iPod button state (1 hold, 2 menu); PMU interrupt (GPIO 0x55)
 * raises and INT register reads; INT2 bit 2/3 = EXTON1 press/release. */
void input_button_line(char *buf) {
    char *p = buf;
    u32 r, a, b;
    *p++ = 'B'; *p++ = 'T'; *p++ = 'N';
    sysic_gpio_diag(GPIO_IRQ_HOLD, &r, &a, &b);
    *p++ = ' '; *p++ = 'h'; p = hx(p, r, 2); *p++ = '/'; p = hx(p, a, 2); p = hx(p, b, 1);
    sysic_gpio_diag(GPIO_IRQ_MENU, &r, &a, &b);
    *p++ = ' '; *p++ = 'm'; p = hx(p, r, 2); *p++ = '/'; p = hx(p, a, 2); p = hx(p, b, 1);
    *p++ = ' '; *p++ = 'k'; p = hx(p, button_bits >> 5, 1);
    sysic_gpio_diag(0x55, &r, &a, &b);
    *p++ = ' '; *p++ = 'P'; *p++ = 'M'; *p++ = 'U';
    *p++ = ' '; p = hx(p, r, 2); *p++ = '/'; p = hx(p, a, 2); p = hx(p, b, 1);
    *p++ = ' '; *p++ = 'r'; p = hx(p, pmu_int_reads, 2);
    *p++ = ' '; *p++ = 'x'; p = hx(p, pmu_int2_state(), 4);
    *p = 0;
}

/* Multitouch protocol position, for the SPI2 status row. */
void mt_cs_debug(u32 *cs, u32 *kept) {
    *cs = mt.cs_count;
    *kept = mt.cs_kept;
}

void mt_debug(u32 *cur, u32 *last, u32 *ind, u32 *size) {
    *cur = mt.cur_cmd;
    *last = mt.last_cmd;
    *ind = mt.buf_ind;
    *size = mt.buf_size;
}
