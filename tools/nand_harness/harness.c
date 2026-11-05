/*
 * harness.c — host test of boot/periph/nand.c + pl080.c
 *
 *   harness DISK.IMG [GENERATOR_NAND_DIR]
 *
 * 1. If GENERATOR_NAND_DIR is given (output of qemu-ios-generate-nand,
 *    branch main, run on a slice of the same HFS volume), every page it
 *    wrote must match what nand_get_page() synthesizes, byte for byte,
 *    except the MBR/GPT pages (their partition size differs by design).
 * 2. Drives the emulation like the kernel's FTL does: ADM init, a single
 *    page read through CPU FIFO reads, then a 16-page read through a PL080
 *    LLI chain into fake kernel RAM; checks against DISK.IMG and the HFS+
 *    volume header / catalog B-tree header node.
 */

#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <string.h>
#include <stdbool.h>
#include <dirent.h>
#include <fcntl.h>
#include <unistd.h>

/* emulation entry points (boot/periph) */
void     nand_init(void);
uint32_t nand_fmc_read(uint32_t off);
void     nand_fmc_write(uint32_t off, uint32_t val);
void     nand_adm_write(uint32_t off, uint32_t val);
void     nand_get_page(uint32_t bank, uint32_t pn, uint8_t *data, uint8_t *spare);
void     pl080_init(void);
void     pl080_write(int dmac, uint32_t off, uint32_t val);
uint32_t pl080_read(int dmac, uint32_t off);

/* host HAL state (hal_host.c) */
extern int      hal_disk_fd;
extern uint64_t hal_disk_size;
extern uint8_t  hal_ram[];
extern uint32_t hal_ram_pa;
extern bool     hal_irq_level[64];

static int failures;
#define CHECK(c, ...) do { if (!(c)) { failures++; printf("FAIL: " __VA_ARGS__); printf("\n"); } } while (0)

static void disk_lba(uint32_t lba, uint8_t *out) {
    memset(out, 0, 2048);
    if (pread(hal_disk_fd, out, 2048, (off_t)lba * 2048) < 0)
        perror("pread");
}

static uint32_t be32(const uint8_t *p) { return (p[0] << 24) | (p[1] << 16) | (p[2] << 8) | p[3]; }
static uint32_t be16(const uint8_t *p) { return (p[0] << 8) | p[1]; }
static void put_be32(uint8_t *p, uint32_t v) { p[0] = v >> 24; p[1] = v >> 16; p[2] = v >> 8; p[3] = v; }
static void put_le32(uint8_t *p, uint32_t v) { p[0] = v; p[1] = v >> 8; p[2] = v >> 16; p[3] = v >> 24; }
static uint8_t *ram(uint32_t pa) { return hal_ram + (pa - hal_ram_pa); }

static void vpn_to_phys(uint32_t vpn, uint32_t *bank, uint32_t *pn) {
    *bank = vpn % 8;
    *pn = (vpn / 1024) * 128 + (vpn / 8) % 128;
}

/* ── 1. generator comparison ─────────────────────────────────────── */

static void compare_generator(const char *dir) {
    uint8_t ref[2048 + 64], data[2048], spare[64];
    uint32_t pages = 0, skipped = 0, bad = 0;
    for (int bank = 0; bank < 8; bank++) {
        char path[512];
        snprintf(path, sizeof(path), "%s/bank%d", dir, bank);
        DIR *d = opendir(path);
        if (!d)
            continue;
        struct dirent *e;
        while ((e = readdir(d))) {
            char *end;
            unsigned long pn = strtoul(e->d_name, &end, 10);
            if (end == e->d_name || strcmp(end, ".page"))
                continue;
            uint32_t vpn = (pn / 128) * 1024 + (pn % 128) * 8 + bank;
            if (vpn >= 206848 && vpn <= 206850) {   /* MBR, GPT header, GPT entry */
                skipped++;
                continue;
            }
            snprintf(path, sizeof(path), "%s/bank%d/%s", dir, bank, e->d_name);
            FILE *f = fopen(path, "rb");
            if (!f || fread(ref, 1, sizeof(ref), f) != sizeof(ref)) {
                printf("FAIL: short page file %s\n", path);
                failures++;
                if (f) fclose(f);
                continue;
            }
            fclose(f);
            if (bank == 0 && pn == 0)           /* this kernel's AND wants C003 */
                put_le32(ref, 0x43303033);
            if (pn == 524288 - 128) {           /* BBT: real all-good bitmap */
                put_le32(ref + 0x34, 512);
                memset(ref + 0x38, 0xFF, 512);
            }
            nand_get_page(bank, pn, data, spare);
            pages++;
            if (memcmp(ref, data, 2048) || memcmp(ref + 2048, spare, 64)) {
                if (bad++ < 5)
                    printf("FAIL: bank %d page %lu (vpn %u) differs (%s)\n", bank, pn, vpn,
                           memcmp(ref, data, 2048) ? "data" : "spare");
                failures++;
            }
        }
        closedir(d);
    }
    printf("generator comparison: %u pages compared, %u MBR/GPT skipped, %u mismatches\n",
           pages, skipped, bad);
    CHECK(pages > 4000, "too few generator pages compared (%u)", pages);
}

/* ── 2. driver-style flow ────────────────────────────────────────── */

#define DATA2 (hal_ram_pa + 0x00000)
#define DATA3 (hal_ram_pa + 0x04000)
#define LLI   (hal_ram_pa + 0x05000)
#define BUF   (hal_ram_pa + 0x10000)
#define CMDB  (DATA2 + 0x1104)

static void adm_cmd(uint32_t cmd, uint32_t n, uint32_t bank, uint32_t page) {
    put_le32(ram(CMDB + 0x24), cmd);
    ram(CMDB + 0x28)[0] = n >> 8;
    ram(CMDB + 0x28)[1] = n;
    ram(CMDB + 0x44)[0] = bank;
    put_be32(ram(CMDB + 0x244), page);
    nand_adm_write(0x04, 2);
}

static void driver_flow(void) {
    uint8_t want[2048], got[2048];

    nand_adm_write(0x84, DATA2 - 0x1000);
    nand_adm_write(0x88, DATA2);
    nand_adm_write(0x8C, DATA3);
    nand_adm_write(0x00, 3);
    CHECK(*(uint32_t *)ram(DATA2) == 0x50, "ADM start-up word");
    CHECK(*(uint32_t *)ram(DATA3 + 28) == 0xA514D3AD, "ADM bank chip IDs");

    /* single page: LBA 3 = first page of the HFS+ volume */
    uint32_t bank, pn;
    vpn_to_phys(206848 + 3, &bank, &pn);
    adm_cmd(0x300, 1, bank, pn);
    CHECK(hal_irq_level[37], "ADM IRQ 37 raised by the command");
    for (int i = 0; i < 512; i++)
        ((uint32_t *)got)[i] = nand_fmc_read(0x80);
    disk_lba(3, want);
    CHECK(!memcmp(got, want, 2048), "single-page FIFO read of LBA 3");
    CHECK(ram(DATA3)[10] == 0xFF, "single-page spare copied to data section 3");
    uint32_t sig = be16(got + 1024);
    CHECK(sig == 0x482B || sig == 0x4858, "HFS+/HFSX signature at LBA 3 (got %04x)", sig);
    printf("volume header: signature %c%c, block size %u\n", got[1024], got[1025],
           be32(got + 1024 + 0x28));
    nand_adm_write(0x04, 0);
    CHECK(!hal_irq_level[37], "ADM IRQ 37 lowered");

    /* 16 pages via PL080 starting at the catalog's first node */
    uint32_t bsize = be32(got + 1024 + 0x28);
    uint32_t cat_block = be32(got + 1024 + 0x110 + 16);
    uint64_t cat_off = (uint64_t)cat_block * bsize;
    uint32_t cat_lba = 3 + cat_off / 2048;
    uint32_t L = cat_lba & ~7u;
    uint32_t V = 206848 + L;
    if ((V % 1024) / 8 == 127) {            /* keep both ops inside one block */
        L -= 8;
        V -= 8;
    }
    vpn_to_phys(V, &bank, &pn);
    for (int b = 0; b < 8; b++) {           /* the driver fills every bank slot */
        ram(CMDB + 0x44)[b] = b;
        put_be32(ram(CMDB + 0x244 + 4 * b), pn);
    }
    adm_cmd(0x200, 16, 0, pn);

    pl080_write(0, 0x030, 1);               /* controller enable */
    for (int i = 0; i < 4; i++) {           /* 4 x 8 KB descriptors */
        uint32_t *w = (uint32_t *)ram(LLI + i * 16);
        w[0] = 0x38A00080;
        w[1] = BUF + i * 8192;
        w[2] = i < 3 ? LLI + (i + 1) * 16 : 0;
        w[3] = 2048 | (2u << 18) | (2u << 21) | (1u << 27) | (i == 3 ? 1u << 31 : 0);
    }
    pl080_write(0, 0x100, 0x38A00080);
    pl080_write(0, 0x104, BUF);
    pl080_write(0, 0x108, LLI + 16);
    pl080_write(0, 0x10C, 2048 | (2u << 18) | (2u << 21) | (1u << 27));
    pl080_write(0, 0x110, 1u | (1u << 15)); /* enable, terminal-count IRQ */

    int ok = 1;
    for (uint32_t i = 0; i < 16; i++) {
        disk_lba(L + i, want);
        if (memcmp(ram(BUF + i * 2048), want, 2048)) {
            ok = 0;
            printf("FAIL: DMA page %u (LBA %u) differs\n", i, L + i);
        }
    }
    CHECK(ok, "16-page PL080 LLI read matches DISK.IMG");
    CHECK(hal_irq_level[16], "DMAC0 IRQ 16 raised at terminal count");
    CHECK(pl080_read(0, 0x004) & 1, "IntTCStatus channel 0");
    pl080_write(0, 0x008, 1);
    CHECK(!hal_irq_level[16], "DMAC0 IRQ 16 cleared");

    uint32_t node = (uint32_t)(cat_off % 2048) + (cat_lba - L) * 2048;
    uint8_t *hdr = ram(BUF + node);
    CHECK(hdr[8] == 1, "catalog node 0 is a B-tree header node (kind %d)", (int8_t)hdr[8]);
    printf("catalog: LBA %u, header node kind %d, node size %u, root node %u, %u leaf records\n",
           cat_lba, (int8_t)hdr[8], be16(hdr + 14 + 18), be32(hdr + 14 + 2),
           be32(hdr + 14 + 6));

    /* beyond the image: erased page with the ECC mark */
    uint8_t data[2048], spare[64];
    vpn_to_phys(206848 + (uint32_t)(hal_disk_size / 2048) + 64, &bank, &pn);
    nand_get_page(bank, pn, data, spare);
    int zeroed = spare[10] == 0xFF && spare[9] == 0 && spare[0] == 0;
    for (int i = 0; i < 2048; i++)
        zeroed &= data[i] == 0;
    CHECK(zeroed, "data-area page beyond DISK.IMG reads zero-filled with the ECC mark");
    {
        uint32_t lba = (uint32_t)(hal_disk_size / 2048), b2, p2;
        vpn_to_phys(206848 + lba, &b2, &p2);
        uint32_t v = 206848 + lba;
        CHECK(b2 == v % 8 && p2 == (v / 1024) * 128 + (v % 1024) / 8,
              "first page past DISK.IMG maps by the touch-1G layout [LBA %u b%u p%x]", lba, b2, p2);
        nand_get_page(1, 0x1181, data, spare);      /* VFL search area, unwritten */
        int ff = 1;
        for (int i = 0; i < 2048; i++)
            ff &= data[i] == 0xFF;
        CHECK(ff && spare[9] == 0xFF, "unwritten page below the data area reads erased (0xFF)");
    }
}

/* ── 3. the command words this kernel's ADM driver really sends ───── */

#define SG   (CMDB + 0xA44)
#define DST  (hal_ram_pa + 0x40000)

static void sg_set(int k, uint32_t pa, uint32_t len) {
    put_be32(ram(SG + 8 * k), pa);
    put_be32(ram(SG + 8 * k + 4), len);
}

static void adm_go(uint32_t word, uint32_t n) {
    put_le32(ram(CMDB + 0x24), word);       /* 0x333B in the high half */
    ram(CMDB + 0x28)[0] = n >> 8;
    ram(CMDB + 0x28)[1] = n;
    nand_adm_write(0x04, 2);
}

static void kernel_words(void) {
    uint8_t want[2048], data[2048], spare[64];
    int ok;
    uint32_t bank, pn;

    nand_adm_write(0x04, 0);
    adm_go(0x333B0100, 0);                  /* stale firmware word: no-op */
    CHECK(hal_irq_level[37], "0x333B0100 completes with IRQ 37");
    nand_adm_write(0x04, 0);

    /* single page read of LBA 3 through a 2-entry scatter list */
    memset(ram(DST), 0xEE, 0x10000);
    vpn_to_phys(206848 + 3, &bank, &pn);
    ram(CMDB + 0x44)[0] = bank;
    put_be32(ram(CMDB + 0x244), pn);
    sg_set(0, DST, 1024);
    sg_set(1, DST + 0x2000, 1024);
    sg_set(2, 0, 0);
    adm_go(0x333B0300, 1);
    disk_lba(3, want);
    CHECK(!memcmp(ram(DST), want, 1024) && !memcmp(ram(DST + 0x2000), want + 1024, 1024),
          "0x333B0300 single read delivered through the scatter list");
    CHECK(ram(DST + 1024)[0] == 0xEE, "scatter list respected entry lengths");
    CHECK(be16(ram(DST + 0x2000)) == 0x482B || be16(ram(DST + 0x2000)) == 0x4858,
          "volume header signature arrives via scatter list");
    nand_adm_write(0x04, 0);

    /* scattered 4-page read: LBAs 100, 7, 3000, 64 into one 8 KB buffer */
    static const uint32_t lbas[4] = { 100, 7, 3000, 64 };
    for (int i = 0; i < 4; i++) {
        vpn_to_phys(206848 + lbas[i], &bank, &pn);
        ram(CMDB + 0x44)[i] = bank;
        put_be32(ram(CMDB + 0x244 + 4 * i), pn);
    }
    sg_set(0, DST, 8192);
    sg_set(1, 0, 0);
    adm_go(0x333B0300, 4);
    ok = 1;
    for (int i = 0; i < 4; i++) {
        disk_lba(lbas[i], want);
        ok &= !memcmp(ram(DST + i * 2048), want, 2048);
    }
    CHECK(ok, "0x333B0300 scattered 4-page read");
    nand_adm_write(0x04, 0);

    /* 0x200: 16 pages = 2 per bank, consecutive LBAs, 3 scatter entries */
    uint32_t L = 1024, V = 206848 + L;
    vpn_to_phys(V, &bank, &pn);
    for (int b = 0; b < 8; b++) {
        ram(CMDB + 0x44)[b] = b;
        put_be32(ram(CMDB + 0x244 + 4 * b), pn);
    }
    sg_set(0, DST, 10240);
    sg_set(1, DST + 0x8000, 14336);
    sg_set(2, DST + 0x10000, 8192);
    sg_set(3, 0, 0);
    adm_go(0x333B0200, 16);
    ok = 1;
    for (uint32_t i = 0; i < 16; i++) {
        uint32_t o = i * 2048, pa;
        disk_lba(L + i, want);
        for (uint32_t b = 0; b < 2048; b++) {
            uint32_t x = o + b;
            pa = x < 10240 ? DST + x : x < 24576 ? DST + 0x8000 + (x - 10240)
                                                 : DST + 0x10000 + (x - 24576);
            if (ram(pa)[0] != want[b]) { ok = 0; break; }
        }
    }
    CHECK(ok, "0x333B0200 16-page read across 3 scatter entries");
    nand_adm_write(0x04, 0);

    /* 0x200 with a different base page per bank slot and n = 12 (not a
     * multiple of 8): page j = slot j % 8, base + j / 8 */
    {
        uint32_t basepn[8];
        for (int b = 0; b < 8; b++) {
            vpn_to_phys(206848 + 64 * (b + 1) * 8, &bank, &basepn[b]);
            ram(CMDB + 0x44)[b] = (b + 3) % 8;              /* shuffled banks */
            put_be32(ram(CMDB + 0x244 + 4 * b), basepn[b]);
        }
        sg_set(0, DST, 12 * 2048);
        sg_set(1, 0, 0);
        ram(CMDB + 0x2C)[0] = 0; ram(CMDB + 0x2C)[1] = 0;   /* no meta */
        adm_go(0x333B0200, 12);
        ok = 1;
        for (uint32_t j = 0; j < 12; j++) {
            uint8_t ref_d[2048], ref_s[64];
            nand_get_page((j % 8 + 3) % 8, basepn[j % 8] + j / 8, ref_d, ref_s);
            ok &= !memcmp(ram(DST + j * 2048), ref_d, 2048);
        }
        CHECK(ok, "0x200 with per-bank base pages and n = 12");
        nand_adm_write(0x04, 0);
    }

    /* single-page ECC reads exactly as the stalled hardware sequence had
     * them (0x0300, n 1, meta 0x0C), with first scatter entries of 0x2000,
     * 0x800, 0 (no list) and a stale 1 MB length: the page must reach the
     * buffer through the PL080 FIFO copy and the spare via data section 3,
     * and nothing may be written past n x (2048 + 12) bytes. */
    {
        static const uint32_t lens[4] = { 0x2000, 0x800, 0, 0x100000 };
        for (int c = 0; c < 4; c++) {
            uint32_t pn2 = 25856 + 0x80, bk = (uint32_t)c % 2;
            uint8_t ref_d[2048], ref_s[64];
            nand_get_page(bk, pn2, ref_d, ref_s);
            memset(ram(DST), 0xEE, 0x3000);
            memset(ram(DATA3), 0xEE, 64);
            ram(CMDB + 0x44)[0] = bk;
            put_be32(ram(CMDB + 0x244), pn2);
            ram(CMDB + 0x2C)[0] = 0; ram(CMDB + 0x2C)[1] = 0x0C;
            sg_set(0, DST, lens[c]);
            /* entry 1: stale (ignored) when entry 0 already covers the
             * transfer; zero, as hardware showed, after a 0x800 entry */
            sg_set(1, DST + 0x2000, lens[c] == 0x800 ? 0 : 0x55555);
            adm_go(0x333B0300, 1);
            pl080_write(0, 0x1A0, 0x38A00080);         /* driver's FIFO drain */
            pl080_write(0, 0x1A4, DST);
            pl080_write(0, 0x1A8, 0);
            pl080_write(0, 0x1AC, 0x200 | (2u << 18) | (2u << 21) | (1u << 27) | (1u << 31));
            pl080_write(0, 0x1B0, 1u | (1u << 15));
            pl080_write(0, 0x008, 0xFF);
            int good = !memcmp(ram(DST), ref_d, 2048) && !memcmp(ram(DATA3), ref_s, 64);
            uint32_t edge = lens[c] && lens[c] < 2060 ? lens[c] : 2060;
            good &= lens[c] == 0 ? ram(DST + 2048)[0] == 0xEE : 1;
            good &= ram(DST + (edge > 2048 ? edge : 2048) + 16)[0] == 0xEE || lens[c] == 0x800;
            good &= ram(DST + 0x2000)[0] == 0xEE;      /* stale entry untouched */
            CHECK(good, "0x0300 single read with first scatter entry %#x", lens[c]);
            nand_adm_write(0x04, 0);
        }
    }

    /* VFL context (block 35, page 0) of every bank, as _LoadVFLCxt reads it */
    for (uint32_t b = 0; b < 8; b++) {
        memset(ram(DATA3), 0x55, 64);
        ram(CMDB + 0x44)[0] = b;
        put_be32(ram(CMDB + 0x244), 35 * 128);
        sg_set(0, DST, 2048);
        sg_set(1, 0, 0);
        adm_go(0x333B0300, 1);
        CHECK(ram(DATA3)[8] == 0 && ram(DATA3)[9] == 0x80,
              "bank %u VFL context spare (single read): %02x %02x", b, ram(DATA3)[8], ram(DATA3)[9]);
        CHECK(*(uint16_t *)ram(DST + 1954) == 35, "bank %u VFL context awInfoBlk[0]", b);
        nand_adm_write(0x04, 0);
    }
    for (uint32_t b = 0; b < 8; b++) {          /* one scattered 8-bank read */
        ram(CMDB + 0x44)[b] = b;
        put_be32(ram(CMDB + 0x244 + 4 * b), 35 * 128);
    }
    memset(ram(DATA3), 0x55, 8 * 12);
    sg_set(0, DST, 8 * 2048);
    sg_set(1, 0, 0);
    adm_go(0x333B0300, 8);
    ok = 1;
    for (uint32_t b = 0; b < 8; b++)
        ok &= ram(DATA3 + b * 12)[8] == 0 && ram(DATA3 + b * 12)[9] == 0x80 &&
              *(uint16_t *)ram(DST + b * 2048 + 1954) == 35;
    CHECK(ok, "8-bank scattered VFL context read carries spare type 0x80 for every bank");
    nand_adm_write(0x04, 0);

    /* bad block table of every bank, every page of the last block */
    for (uint32_t b = 0; b < 8 * 128; b++) {
        nand_get_page(b / 128, 524288 - 128 + b % 128, data, spare);
        CHECK(!memcmp(data, "DEVICEINFOBBT\0\0\0", 16) && *(uint32_t *)(data + 0x34) == 512 &&
              data[0x38] == 0xFF && data[0x38 + 511] == 0xFF && data[0x38 + 512] == 0,
              "bank %u page %u BBT bitmap", b / 128, 524160 + b % 128);
    }

    /* the driver's PL080 drains the FIFO over the scatter buffer after the
     * ADM command, poking FMDNUM/FMCTRL0 first: data must stay correct */
    extern int emu_fil_read(uint32_t, uint32_t, uint8_t *, uint8_t *);
    static const uint32_t probe[][2] = { {0, 0}, {3, 4480}, {6, 4480}, {0, 524160},
                                         {0, 524162}, {3, 1617}, {7, 13000},
                                         {5, 25856}, {2, 25900}, {7, 26001} };
    for (unsigned i = 0; i < sizeof(probe) / sizeof(probe[0]); i++) {
        uint8_t d2[2048], s2[64];
        nand_get_page(probe[i][0], probe[i][1], data, spare);
        emu_fil_read(probe[i][0], probe[i][1], d2, s2);
        CHECK(!memcmp(d2, data, 2048) && !memcmp(s2, spare, 64),
              "ADM + PL080 FIFO copy delivers bank %u page %u intact", probe[i][0], probe[i][1]);
    }

    /* replay of the hardware's last commands before the stall (#DB2-#DB9):
     * single ECC reads of row 0xAB00 on banks 4-7 with 0x800/0x1000 lists,
     * a 0x200 of 8 pages from 0xAB01 with 5 entries (0x4800), then single
     * reads of row 0xAB02 on banks 0-2; each drained by the driver's PL080
     * and acknowledged.  #DB9 read LBA 143378, past the v1 DISK.IMG's
     * volume end (the alternate-VH probe region). */
    {
        struct { uint32_t op, n, bank, page, sg[5]; } rp[8] = {
            { 0x300, 1, 4, 0xAB00, { 0x800 } },  { 0x300, 1, 5, 0xAB00, { 0x1000 } },
            { 0x300, 1, 6, 0xAB00, { 0x800 } },  { 0x300, 1, 7, 0xAB00, { 0x1000 } },
            { 0x200, 8, 0, 0xAB01, { 0x1000, 0x1000, 0x1000, 0x1000, 0x800 } },
            { 0x300, 1, 0, 0xAB02, { 0x800 } },  { 0x300, 1, 1, 0xAB02, { 0x1000 } },
            { 0x300, 1, 2, 0xAB02, { 0x800 } },
        };
        int all = 1;
        for (int r = 0; r < 8; r++) {
            uint32_t pa = DST;
            for (int b = 0; b < 8; b++) {
                ram(CMDB + 0x44)[b] = rp[r].op == 0x200 ? b : rp[r].bank;
                put_be32(ram(CMDB + 0x244 + 4 * b), rp[r].page);
            }
            for (int k = 0; k < 5; k++) {
                sg_set(k, pa, rp[r].sg[k]);
                pa += 0x1000;
            }
            sg_set(5, 0, 0);
            ram(CMDB + 0x2C)[0] = 0; ram(CMDB + 0x2C)[1] = 0x0C;
            adm_go(0x333B0000 | rp[r].op, rp[r].n);
            int early = !hal_irq_level[37];     /* completion raised at once */
            /* <= 4095 words per descriptor: 8 pages need a second one */
            uint32_t words = rp[r].n * 512, first = words > 2048 ? 2048 : words;
            if (words > first) {
                uint32_t *w = (uint32_t *)ram(LLI);
                w[0] = 0x38A00080;
                w[1] = DST + 0x10000 + first * 4;
                w[2] = 0;
                w[3] = (words - first) | (2u << 18) | (2u << 21) | (1u << 27) | (1u << 31);
            }
            pl080_write(0, 0x1A0, 0x38A00080);
            pl080_write(0, 0x1A4, DST + 0x10000);
            pl080_write(0, 0x1A8, words > first ? LLI : 0);
            pl080_write(0, 0x1AC, first | (2u << 18) | (2u << 21) | (1u << 27) |
                                  (words > first ? 0 : 1u << 31));
            pl080_write(0, 0x1B0, 1u | (1u << 15));
            int done = hal_irq_level[37] && hal_irq_level[16];
            pl080_write(0, 0x008, 0xFF);
            int ok2 = 1;
            for (uint32_t j = 0; j < rp[r].n; j++) {
                uint8_t ref_d[2048], ref_s[64];
                uint32_t bk = rp[r].op == 0x200 ? j % 8 : rp[r].bank;
                nand_get_page(bk, rp[r].page + (rp[r].op == 0x200 ? j / 8 : 0), ref_d, ref_s);
                ok2 &= !memcmp(ram(DST + 0x10000 + j * 2048), ref_d, 2048);
            }
            nand_adm_write(0x04, 0);
            int acked = !hal_irq_level[37];
            if (early || !done || !ok2 || !acked) {
                all = 0;
                printf("FAIL: replay #%X op %x: early %d done %d data %d acked %d\n",
                       0xDB2 + r, rp[r].op, early, done, ok2, acked);
            }
        }
        CHECK(all, "replay of #DB2-#DB9 completes every command after its drain");
    }

    /* FIL ID page carries this driver's signature */
    nand_get_page(0, 0, data, spare);
    CHECK(*(uint32_t *)data == 0x43303033, "FIL ID page is C003");
}

int main(int argc, char **argv) {
    if (argc < 2) {
        fprintf(stderr, "usage: %s DISK.IMG [GENERATOR_NAND_DIR]\n", argv[0]);
        return 2;
    }
    hal_disk_fd = open(argv[1], O_RDONLY);
    if (hal_disk_fd < 0) {
        perror(argv[1]);
        return 2;
    }
    hal_disk_size = lseek(hal_disk_fd, 0, SEEK_END);

    nand_init();
    pl080_init();
    if (argc > 2)
        compare_generator(argv[2]);
    driver_flow();
    kernel_words();

    /* dmac1's paced SPI2 RX channel must have moved only the bytes the
     * device offered (3 per NAND read), never run away. */
    extern uint32_t spi2_rx_read, spi2_rx_left;
    printf("dmac1 paced SPI2 RX: %u bytes read, %u left\n", spi2_rx_read, spi2_rx_left);
    if (!spi2_rx_read || spi2_rx_left) {
        printf("FAIL: paced DMA did not drain the offered bytes\n");
        failures++;
    }

    /* A memory->memory channel whose LLI points back at itself must not
     * spin: each enable / heartbeat / register poll runs one slice. */
    {
        extern uint32_t pl080_read(int dmac, uint32_t off);
        extern void pl080_tick(void);
        uint32_t base = hal_ram_pa + 0xF0000, desc = base + 0x1000;
        put_le32(ram(desc + 0), base);              /* src */
        put_le32(ram(desc + 4), base + 0x2000);     /* dst */
        put_le32(ram(desc + 8), desc);              /* lli: itself */
        put_le32(ram(desc + 12), 0xFFF | (2u << 18) | (2u << 21));
        pl080_write(0, 0x030, 1);
        pl080_write(0, 0x1E0, base);
        pl080_write(0, 0x1E4, base + 0x2000);
        pl080_write(0, 0x1E8, desc);
        pl080_write(0, 0x1EC, 0xFFF | (2u << 18) | (2u << 21));
        pl080_write(0, 0x1F0, 1);                   /* enable ch 7 */
        int still = (pl080_read(0, 0x01C) >> 7) & 1;
        pl080_tick();
        still &= (pl080_read(0, 0x01C) >> 7) & 1;
        pl080_write(0, 0x1F0, 0);                   /* driver stops it */
        int stopped = !((pl080_read(0, 0x01C) >> 7) & 1);
        CHECK(still && stopped, "self-looping memory LLI runs in slices, stays pending");
    }

    printf(failures ? "RESULT: %d failure(s)\n" : "RESULT: all checks passed\n", failures);
    return failures != 0;
}
