/*
 * tmio.c -- TMIO SD card controller driver for the 3DS ARM9
 *
 * Drives the TMIO SDMMC controller at 0x10006000 to read sectors
 * from the SD card.  Based on the Normmatt/devkitPro sdmmc.c driver
 * register usage and data path.
 *
 * Hardware reference: 3DBrew SDMMC Registers (public documentation).
 */

#include "tmio.h"

/* ================================================================
 * TMIO register definitions (offsets from 0x10006000)
 * ================================================================ */

#define TMIO_BASE           0x10006000

#define REG16(off)          (*(volatile u16 *)(TMIO_BASE + (off)))
#define REG32(off)          (*(volatile u32 *)(TMIO_BASE + (off)))

/* 16-bit control/status registers */
#define SD_CMD              REG16(0x000)
#define SD_PORTSEL          REG16(0x002)
#define SD_ARG0             REG16(0x004)
#define SD_ARG1             REG16(0x006)
#define SD_STOP             REG16(0x008)
#define SD_BLKCOUNT         REG16(0x00A)
#define SD_RESP0            REG16(0x00C)
#define SD_RESP1            REG16(0x00E)
#define SD_RESP2            REG16(0x010)
#define SD_RESP3            REG16(0x012)
#define SD_RESP4            REG16(0x014)
#define SD_RESP5            REG16(0x016)
#define SD_RESP6            REG16(0x018)
#define SD_RESP7            REG16(0x01A)
#define SD_ISTAT0           REG16(0x01C)
#define SD_ISTAT1           REG16(0x01E)
#define SD_IMASK0           REG16(0x020)
#define SD_IMASK1           REG16(0x022)
#define SD_CLKCTL           REG16(0x024)
#define SD_BLKLEN           REG16(0x026)
#define SD_OPTION           REG16(0x028)
#define SD_SOFTRST          REG16(0x0E0)

#define SD_ISTAT32          REG32(0x01C)

/* 32-bit data path registers */
#define SD_DATACTL32        REG16(0x100)
#define SD_DATA32_BLKLEN    REG16(0x104)
#define SD_DATA32_BLKCNT    REG16(0x108)
#define SD_FIFO32           REG32(0x10C)

/* ================================================================
 * Status bit definitions
 * ================================================================ */

/* ISTAT0 / STAT0 (offset 0x01C, lower 16 bits of ISTAT32) */
#define STAT0_CMDRESPEND    0x0001u
#define STAT0_DATAEND       0x0004u

/* ISTAT1 / STAT1 (offset 0x01E, upper 16 bits of ISTAT32) */
#define STAT1_RXRDY         0x0100u
#define STAT1_CMD_BUSY      0x4000u

/* DATACTL32 (offset 0x100) */
#define DATACTL32_RXRDY     0x0100u  /* Bit 8: data ready in 32-bit FIFO */
#define DATACTL32_BUSY      0x0800u  /* Bit 11: cleared after block read */
#define DATACTL32_ENABLE    0x0002u  /* Bit 1: enable 32-bit FIFO */

/* ================================================================
 * SD_CMD register flag fields
 * ================================================================ */

#define CMD_RESP_R1         (4u << 8)   /* R1/R5/R6/R7: 48-bit resp    */
#define CMD_RESP_R2         (6u << 8)   /* 136-bit response (CID/CSD)  */
#define CMD_RESP_R1B        (5u << 8)   /* R1b/R5b: 48-bit + busy      */
#define CMD_DATA            (1u << 11)  /* Transfer has data payload   */
#define CMD_READ            (1u << 12)  /* Data direction: read        */
#define CMD_MULTI           (1u << 13)  /* Multi-block transfer        */

/* ================================================================
 * Internal state
 * ================================================================ */

static u16 card_rca;
static u32 last_istat;

u32 tmio_last_istat(void)
{
    return last_istat;
}

/* ================================================================
 * Low-level helpers
 * ================================================================ */

static void tmio_clear_status(void)
{
    SD_ISTAT0 = 0;
    SD_ISTAT1 = 0;
}

static void tmio_set_arg(u32 arg)
{
    SD_ARG0 = (u16)(arg & 0xFFFF);
    SD_ARG1 = (u16)(arg >> 16);
}

/*
 * The controller at 0x10006000 serves both the SD slot (port 0) and the
 * console's internal eMMC (port 1).  This driver must only ever talk to the
 * SD card: every command goes through this guard, which forces port 0 and
 * refuses to proceed if the selection doesn't read back as port 0.  The
 * driver has no write commands (CMD24/CMD25) and never sends eMMC commands.
 */
static bool sd_port_guard(void)
{
    if (SD_PORTSEL & 3u)
        SD_PORTSEL = SD_PORTSEL & ~3u;
    return (SD_PORTSEL & 3u) == 0;
}

static void tmio_send_cmd(u16 cmd, u32 arg)
{
    if (!sd_port_guard())
        return;
    tmio_clear_status();
    tmio_set_arg(arg);
    SD_CMD = cmd;

    for (u32 t = 0; t < 10000000; t++) {
        if (SD_ISTAT0 & STAT0_CMDRESPEND)
            break;
    }
}

static u32 tmio_get_resp0(void)
{
    u32 lo = SD_RESP0;
    u32 hi = SD_RESP1;
    return lo | (hi << 16);
}

/* ================================================================
 * SD card initialization
 * ================================================================ */

bool tmio_init(bool skip_card_init)
{
    if (skip_card_init) {
        SD_PORTSEL = 0x0000;
        SD_IMASK0 = 0;
        SD_IMASK1 = 0;
        SD_BLKLEN        = 512;
        SD_DATA32_BLKLEN = 512;
        SD_DATA32_BLKCNT = 1;
        SD_DATACTL32 = (SD_DATACTL32 & ~DATACTL32_BUSY) | DATACTL32_ENABLE;
        tmio_clear_status();
        return true;
    }

    /* ---- Full controller setup ---- */

    SD_PORTSEL = 0x0000;
    SD_SOFTRST = 0;
    SD_SOFTRST = 1;
    SD_IMASK0 = 0;
    SD_IMASK1 = 0;
    SD_BLKLEN        = 512;
    SD_DATA32_BLKLEN = 512;
    SD_DATA32_BLKCNT = 1;
    SD_DATACTL32 = (SD_DATACTL32 & ~DATACTL32_BUSY) | DATACTL32_ENABLE;
    SD_STOP   = 0;
    SD_CLKCTL = 0x0180;    /* ~400 kHz */
    SD_OPTION = 0x80D0;    /* 1-bit bus width */

    /* ---- Card initialization ---- */
    {
        /* CMD0: GO_IDLE_STATE */
        tmio_send_cmd(0, 0);

        /* CMD8: SEND_IF_COND */
        tmio_send_cmd(8 | CMD_RESP_R1, 0x000001AA);
        {
            u32 resp = tmio_get_resp0();
            if ((resp & 0xFFF) != 0x1AA)
                return false;
        }

        /* ACMD41: SD_SEND_OP_COND */
        for (u32 i = 0; i < 1000; i++) {
            tmio_send_cmd(55 | CMD_RESP_R1, 0);
            tmio_send_cmd(41 | CMD_RESP_R1, 0x40FF8000);
            u32 ocr = tmio_get_resp0();
            if (ocr & 0x80000000u)
                goto acmd41_done;
        }
        return false;
    acmd41_done:

        /* CMD2: ALL_SEND_CID */
        tmio_send_cmd(2 | CMD_RESP_R2, 0);

        /* CMD3: SEND_RELATIVE_ADDR */
        tmio_send_cmd(3 | CMD_RESP_R1, 0);
        card_rca = (u16)(tmio_get_resp0() >> 16);

        /* CMD7: SELECT_CARD */
        tmio_send_cmd(7 | CMD_RESP_R1B, (u32)card_rca << 16);

        /* ACMD6: SET_BUS_WIDTH = 4-bit */
        tmio_send_cmd(55 | CMD_RESP_R1, (u32)card_rca << 16);
        tmio_send_cmd(6 | CMD_RESP_R1, 2);
    }

    /* Switch to 4-bit, high-speed */
    SD_OPTION = 0x00D0;
    SD_CLKCTL = 0x0100;

    return true;
}

/* ================================================================
 * Sector reading — polls DATACTL32 bit 8, not ISTAT RXRDY
 * ================================================================ */

bool tmio_read_sectors(u32 sector, u32 count, void *buf)
{
    if (count == 0)
        return true;
    if (!sd_port_guard())
        return false;

    SD_STOP          = 0x0100;
    SD_BLKCOUNT      = (u16)count;
    SD_DATA32_BLKCNT = (u16)count;
    SD_DATA32_BLKLEN = 512;
    tmio_clear_status();
    tmio_set_arg(sector);

    u16 cmd;
    if (count == 1)
        cmd = 17 | CMD_RESP_R1 | CMD_DATA | CMD_READ;
    else
        cmd = 18 | CMD_RESP_R1 | CMD_DATA | CMD_READ | CMD_MULTI;
    SD_CMD = cmd;

    u32 *dst = (u32 *)buf;
    for (u32 b = 0; b < count; b++) {
        /* Poll DATACTL32 bit 8 for data ready (not ISTAT RXRDY) */
        for (u32 t = 0; t < 10000000; t++) {
            if (SD_DATACTL32 & DATACTL32_RXRDY)
                goto got_block;
        }
        last_istat = ((u32)SD_DATACTL32 << 16) | SD_ISTAT0;
        return false;

    got_block:
        /* Clear RXRDY in STAT1 before reading */
        SD_ISTAT1 = SD_ISTAT1 & ~STAT1_RXRDY;

        /* Read 512 bytes (128 x 32-bit words) from FIFO */
        for (u32 w = 0; w < 128; w++)
            *dst++ = SD_FIFO32;

        /* Clear DATACTL32 busy bit after each block */
        SD_DATACTL32 = SD_DATACTL32 & ~DATACTL32_BUSY;
    }

    /* Wait for DATAEND */
    for (u32 t = 0; t < 10000000; t++) {
        if (SD_ISTAT0 & STAT0_DATAEND)
            break;
    }

    tmio_clear_status();
    return true;
}
