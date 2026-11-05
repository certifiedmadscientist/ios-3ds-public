/*
 * hal_host.c — host implementation of boot/periph/nand_hal.h for the harness.
 * DISK.IMG is read with pread(); "physical memory" is a 1 MB array at
 * PA 0x21000000; interrupt lines are recorded.
 */

#include <stdio.h>
#include <stdint.h>
#include <stdbool.h>
#include <string.h>
#include <unistd.h>
#include <stdlib.h>

#define SLOT_BYTES (32u * 2048u)
#define SLOTS      32u
#define RAM_SIZE   (1u << 20)

int      hal_disk_fd = -1;
uint64_t hal_disk_size;
uint8_t  hal_ram[RAM_SIZE];
uint32_t hal_ram_pa = 0x21000000u;
bool     hal_irq_level[64];

static uint8_t slots[SLOTS][SLOT_BYTES];

uint8_t *nhal_slot(uint32_t n) { return slots[n % SLOTS]; }

static uint8_t overlay[7680u * (2048u + 64u)];
uint8_t *nhal_overlay(void) { return overlay; }

uint32_t nhal_disk_sectors(void) { return (uint32_t)(hal_disk_size / 512); }

bool nhal_fill(uint32_t n, uint32_t sector, uint32_t count) {
    if (count == 0 || count > 128 || (uint64_t)(sector + count) * 512 > hal_disk_size)
        return false;
    return pread(hal_disk_fd, slots[n % SLOTS], count * 512, (off_t)sector * 512) ==
           (ssize_t)(count * 512);
}

static bool in_ram(uint32_t pa, uint32_t len) {
    return pa >= hal_ram_pa && len <= RAM_SIZE && pa - hal_ram_pa <= RAM_SIZE - len;
}

bool nhal_phys_read(uint32_t pa, void *dst, uint32_t len) {
    if (!in_ram(pa, len))
        return false;
    memcpy(dst, hal_ram + (pa - hal_ram_pa), len);
    return true;
}

bool nhal_phys_write(uint32_t pa, const void *src, uint32_t len) {
    if (!in_ram(pa, len))
        return false;
    memcpy(hal_ram + (pa - hal_ram_pa), src, len);
    return true;
}

void nhal_irq(int line, bool level) {
    if (line >= 0 && line < 64)
        hal_irq_level[line] = level;
}

void nhal_log(const char *msg, uint32_t val) { printf("%s%08x\n", msg, val); }
uint32_t nhal_ticks(void) { static uint32_t t; return t += 50; }
void nhal_puts(const char *line) { if (getenv("NAND_VERBOSE")) printf("%s\n", line); }

/* pl080.c reaches other devices through the peripheral dispatcher.  The
 * only other device here is a fake SPI2 RX register holding 3 bytes per
 * NAND read, to check that dmac1's paced channels move just that. */
#define FAKE_SPI2_RX 0x3D200020u
uint32_t spi2_rx_left, spi2_rx_read;
uint32_t periph_dispatch(uint32_t addr, uint32_t value, bool is_write, uint32_t width) {
    (void)value; (void)width;
    if (addr == FAKE_SPI2_RX && !is_write && spi2_rx_left) {
        spi2_rx_left--;
        spi2_rx_read++;
        return 0x5A;
    }
    printf("harness: unexpected device access %08x\n", addr);
    return 0;
}
uint32_t periph_dma_avail(uint32_t pa, bool to_device) {
    return (!to_device && pa == FAKE_SPI2_RX) ? spi2_rx_left : 0;
}

/* ── one FIL page read the way the kernel's ADM driver does it ─────────
 * ADM read 0x333B0300 with a one-entry 4 KB scatter buffer, the driver
 * then touching FMDNUM/FMCTRL0, then PL080 channel 5 copying 0x800 bytes
 * from the FMC FIFO over the same buffer.  Returns what lands in memory:
 * data = buffer[0..2048), spare = data section 3. */
void nand_adm_write(uint32_t off, uint32_t val);
void nand_fmc_write(uint32_t off, uint32_t val);
uint32_t nand_fmc_read(uint32_t off);
void pl080_write(int dmac, uint32_t off, uint32_t val);

#define EMU_DATA2 0x21000000u
#define EMU_DATA3 0x21004000u
#define EMU_DST   0x21010000u

static void put_be32(uint8_t *p, uint32_t v) { p[0] = v >> 24; p[1] = v >> 16; p[2] = v >> 8; p[3] = v; }

int emu_fil_read(uint32_t bank, uint32_t page, uint8_t *data, uint8_t *spare) {
    static int ready;
    uint8_t *blk = hal_ram + (EMU_DATA2 - hal_ram_pa) + 0x1104;
    if (!ready) {
        nand_adm_write(0x88, EMU_DATA2);
        nand_adm_write(0x8C, EMU_DATA3);
        pl080_write(0, 0x030, 1);
        /* dmac1 ch 0: SPI2 RX -> RAM, 0x100 bytes, descriptor looping on
         * itself (what made the eager model run away). */
        static uint32_t lli[4];
        uint32_t lli_pa = EMU_DST + 0x2000;
        lli[0] = FAKE_SPI2_RX; lli[1] = EMU_DST + 0x3000; lli[2] = lli_pa;
        lli[3] = 0x100 | (1u << 27) | (1u << 31);
        memcpy(hal_ram + (lli_pa - hal_ram_pa), lli, 16);
        pl080_write(1, 0x030, 1);
        pl080_write(1, 0x100, lli[0]);
        pl080_write(1, 0x104, lli[1]);
        pl080_write(1, 0x108, lli[2]);
        pl080_write(1, 0x10C, lli[3]);
        pl080_write(1, 0x110, 1u | (1u << 15));
        ready = 1;
    }
    memset(hal_ram + (EMU_DST - hal_ram_pa), 0xA5, 0x1000);
    memset(hal_ram + (EMU_DATA3 - hal_ram_pa), 0xA5, 64);
    blk[0x24] = 0x00; blk[0x25] = 0x03; blk[0x26] = 0x3B; blk[0x27] = 0x33;
    blk[0x28] = 0; blk[0x29] = 1;
    blk[0x44] = bank;
    put_be32(blk + 0x244, page);
    put_be32(blk + 0xA44, EMU_DST);
    put_be32(blk + 0xA48, 0x1000);
    put_be32(blk + 0xA4C, 0);
    put_be32(blk + 0xA50, 0);
    spi2_rx_left = 3;                       /* SPI2 has 3 bytes for DMA */
    extern void pl080_service(void);
    pl080_service();
    nand_adm_write(0x04, 2);
    nand_fmc_write(0x30, 0x7FF);            /* driver pokes the FMC ... */
    nand_fmc_write(0x00, 0);
    nand_fmc_write(0x08, 0x70);             /* ... and polls status by PIO */
    if (nand_fmc_read(0x80) != 0x40)
        printf("harness: FIFO status read gave the wrong word\n");
    pl080_write(0, 0x1A0, 0x38A00080);      /* ... then drains the FIFO (ch 5) */
    pl080_write(0, 0x1A4, EMU_DST);
    pl080_write(0, 0x1A8, 0);
    pl080_write(0, 0x1AC, 0x200 | (2u << 18) | (2u << 21) | (1u << 27) | (1u << 31));
    pl080_write(0, 0x1B0, 1u | (1u << 15));
    pl080_write(0, 0x008, 0xFF);
    nand_adm_write(0x04, 0);
    memcpy(data, hal_ram + (EMU_DST - hal_ram_pa), 2048);
    memcpy(spare, hal_ram + (EMU_DATA3 - hal_ram_pa), 64);
    return 0;
}
