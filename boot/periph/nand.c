/*
 * nand.c — S5L8900 NAND emulation: FMC, RS-ECC, ADM and the page source.
 *
 * Register behaviour follows qemu-ios (branch ipod_touch_1g:
 * hw/arm/ipod_touch_nand.c, ipod_touch_nand_ecc.c, ipod_touch_adm.c), which
 * boots iPhone OS 1.x to SpringBoard from such a NAND.  Page contents follow
 * qemu-ios-generate-nand (branch main, a893e27 layout); see nand.h.
 *
 * Writes behave like qemu-ios: a programmed page lands in the page buffer and
 * is never persisted, so a later read of another page drops it.
 *
 * This kernel's ADM firmware (CalmADMFMCFirmware-17) is newer than the one
 * qemu-ios models.  The driver (AppleS5L8900XADMFMC, ~0xC04AC248) stores the
 * opcode as a halfword at +0x24 (the halfword at +0x26 is another field),
 * and for reads also a scatter list of {BE phys addr, BE length} at +0xA44
 * that the firmware fills directly; the qemu-style FIFO path stays too.
 * Opcodes: 0x200/0x300 read, 0x400/0x500 program, 0x600 erase.
 */

#include "nand.h"
#include "nand_hal.h"
#include "pl080.h"

#define NOLDP __attribute__((optimize("no-tree-loop-distribute-patterns")))

/* FMC registers */
#define FMCTRL0   0x000
#define FMCTRL1   0x004
#define FMCMD     0x008
#define FMADDR0   0x00C
#define FMADDR1   0x010
#define FMANUM    0x02C
#define FMDNUM    0x030
#define FMCSTAT   0x048
#define FMFIFO    0x080
#define RSCTRL    0x100

#define CMD_READ        0x30
#define CMD_READSTATUS  0x70
#define CMD_ID          0x90
#define NAND_CHIP_ID    0xA514D3ADu

/* ECC registers */
#define ECC_START     0x0C
#define ECC_STATUS    0x10
#define ECC_CLEARINT  0x40

/* ADM registers and command block (in the kernel's data section 2) */
#define ADM_CTRL      0x00
#define ADM_CTRL2     0x04
#define ADM_CODE_SEC  0x50
#define ADM_DATA1_SEC 0x84
#define ADM_DATA2_SEC 0x88
#define ADM_DATA3_SEC 0x8C
#define ADM_CMDBLK    0x1104
#define ADM_CMD       (ADM_CMDBLK + 0x24)
#define ADM_NPAGES    (ADM_CMDBLK + 0x28)   /* u16, big-endian */
#define ADM_BANKS     (ADM_CMDBLK + 0x44)   /* u8 per page     */
#define ADM_CEMAP     (ADM_CMDBLK + 0x34)   /* u8 CE per slot  */
#define ADM_PAGES     (ADM_CMDBLK + 0x244)  /* u32 BE per page */
#define ADM_SGLIST    (ADM_CMDBLK + 0xA44)  /* {u32 BE pa, u32 BE len} */
#define ADM_SG_MAX    256u
#define ADM_META      (ADM_CMDBLK + 0x2C)   /* u16 BE metadata bytes per page */

/* NAND geometry and generator layout */
#define BANKS             8u
#define PAGES_PER_BLOCK   128u
#define PAGES_PER_SUBLOCK 1024u
#define PAGES_PER_BANK    524288u
#define BLOCKS_PER_BANK   (PAGES_PER_BANK / PAGES_PER_BLOCK)
/* qemu-ios's generator writes 'C002'; this kernel's AND driver is 'C003'
 * and treats any other low byte as a format epoch to upgrade. */
#define FIL_ID            0x43303033u
#define BBT_PN            (PAGES_PER_BANK - PAGES_PER_BLOCK)
#define VFL_CXT_PN        (35u * PAGES_PER_BLOCK)
#define FTL_CXT_VPN       (201u * PAGES_PER_SUBLOCK)        /* 205824 */
#define FTL_MAP_PAGES     18u
#define FTL_META_VPN      (202u * PAGES_PER_SUBLOCK - 1u)   /* 206847 */

#define MULTI_MAX         512u

static struct {
    u32 fmctrl0, fmctrl1, fmaddr0, fmaddr1, fmanum, fmdnum, rsctrl, cmd;
    bool reading_spare, reading_multi, is_writing;
    bool adm_fifo;              /* FIFO serves the last ADM read command */
    u32  fifo_pos, fifo_n;

    int  cur_bank_reading;
    u8   banks_to_read[MULTI_MAX];
    u32  pages_to_read[MULTI_MAX];
    int  buf_bank;
    u32  buf_page;
    u32  adm_code, adm_data1, adm_data2, adm_data3;
} s;

static u8  page_buf[NAND_PAGE_BYTES] ALIGNED(4);
static u8  spare_buf[NAND_SPARE_BYTES] ALIGNED(4);
static u32 slot_tag[NHAL_SLOTS];        /* chunk + 1, 0 = empty */
static u32 stat_pages, stat_misses;

static void note_err(u32 code);
static void note_post_read(u32 pa, u32 v);
static void irq(int line, bool level);


/* ── small helpers ─────────────────────────────────────────────────── */

static NOLDP void zero(void *p, u32 n) {
    u8 *d = p;
    for (u32 i = 0; i < n; i++)
        d[i] = 0;
}

static NOLDP void copy(void *dst, const void *src, u32 n) {
    u8 *d = dst;
    const u8 *q = src;
    for (u32 i = 0; i < n; i++)
        d[i] = q[i];
}

static void put16(u8 *p, u32 off, u32 v) { p[off] = v; p[off + 1] = v >> 8; }
static void put32(u8 *p, u32 off, u32 v) {
    p[off] = v; p[off + 1] = v >> 8; p[off + 2] = v >> 16; p[off + 3] = v >> 24;
}
static u32 get32(const u8 *p) {
    return p[0] | (p[1] << 8) | (p[2] << 16) | ((u32)p[3] << 24);
}

static u32 phys_be32(u32 pa) {
    u8 b[4] = { 0, 0, 0, 0 };
    nhal_phys_read(pa, b, 4);
    return ((u32)b[0] << 24) | (b[1] << 16) | (b[2] << 8) | b[3];
}
static u32 phys_be16(u32 pa) {
    u8 b[2] = { 0, 0 };
    nhal_phys_read(pa, b, 2);
    return (b[0] << 8) | b[1];
}
static u32 phys_u8(u32 pa) {
    u8 b = 0;
    nhal_phys_read(pa, &b, 1);
    return b;
}
static u32 phys_le32(u32 pa) {
    u8 b[4] = { 0, 0, 0, 0 };
    nhal_phys_read(pa, b, 4);
    return get32(b);
}

/* ── write overlay ─────────────────────────────────────────────────── */

/* Programmed pages live in nhal_overlay() records (2048 data + 64 spare),
 * indexed by a chained hash on (bank, page).  An erase drops the block's
 * records and marks the block erased, so its unprogrammed pages read as
 * erased NAND (all 0xFF) instead of the synthesized/disk contents.  When
 * the store is full a program is dropped and counted (never overwrites
 * another page). */
#define OVL_NONE    0xFFFFu
#define OVL_BUCKETS 4096u
static u16 ovl_head[OVL_BUCKETS];
static u16 ovl_next[NHAL_OVL_PAGES];
static u32 ovl_key[NHAL_OVL_PAGES];
static u16 ovl_free;
static u32 ovl_erased[BANKS * BLOCKS_PER_BANK / 32];
static struct { u32 used, programs, erases, full; } ovl;

static void ovl_init(void) {
    for (u32 i = 0; i < OVL_BUCKETS; i++)
        ovl_head[i] = OVL_NONE;
    for (u32 i = 0; i < NHAL_OVL_PAGES; i++)
        ovl_next[i] = i + 1 < NHAL_OVL_PAGES ? (u16)(i + 1) : OVL_NONE;
    ovl_free = 0;
    zero(ovl_erased, sizeof(ovl_erased));
    zero(&ovl, sizeof(ovl));
}

static u32 ovl_hash(u32 key) {
    return (key ^ (key >> 12) ^ (key >> 7)) % OVL_BUCKETS;
}

static u32 ovl_find(u32 bank, u32 page) {
    u32 key = (bank << 20) | page;
    static bool shown;
    u32 it = 0;
    for (u32 i = ovl_head[ovl_hash(key)]; i != OVL_NONE; i = ovl_next[i]) {
        if (ovl_key[i] == key)
            return i;
        if (++it > NHAL_OVL_PAGES) {    /* cycle in the hash chain */
            hang_cap(&shown, "[NAND] cap ovl", bank, page, i, it);
            break;
        }
    }
    return OVL_NONE;
}

static u8 *ovl_rec(u32 i) {
    return nhal_overlay() + i * NHAL_OVL_REC;
}

static bool block_erased(u32 bank, u32 page) {
    u32 b = bank * BLOCKS_PER_BANK + page / PAGES_PER_BLOCK;
    return b < BANKS * BLOCKS_PER_BANK && (ovl_erased[b / 32] >> (b % 32) & 1);
}

/* Program one page: data (2048 bytes) and the first `meta` spare bytes;
 * the rest of the spare reads back as 0xFF, as on NAND. */
static void ovl_program(u32 bank, u32 page, const u8 *data, const u8 *meta,
                        u32 metalen) {
    u32 i = ovl_find(bank, page);
    if (i == OVL_NONE) {
        if (ovl_free == OVL_NONE) {
            if (ovl.full++ == 0)
                nhal_log("[NAND] write overlay full, dropping page ", page);
            note_err(0xE4);
            return;
        }
        i = ovl_free;
        ovl_free = ovl_next[i];
        u32 key = (bank << 20) | page, h = ovl_hash(key);
        ovl_key[i] = key;
        ovl_next[i] = ovl_head[h];
        ovl_head[h] = (u16)i;
        ovl.used++;
    }
    u8 *r = ovl_rec(i);
    copy(r, data, NAND_PAGE_BYTES);
    for (u32 k = 0; k < NAND_SPARE_BYTES; k++)
        r[NAND_PAGE_BYTES + k] = k < metalen ? meta[k] : 0xFF;
    ovl.programs++;
    s.buf_page = 0xFFFFFFFF;            /* page_buf may hold the old copy */
}

static void ovl_erase(u32 bank, u32 page) {
    u32 first = page - page % PAGES_PER_BLOCK;
    u32 b = bank * BLOCKS_PER_BANK + first / PAGES_PER_BLOCK;
    if (b >= BANKS * BLOCKS_PER_BANK)
        return;
    for (u32 p = first; p < first + PAGES_PER_BLOCK; p++) {
        u32 key = (bank << 20) | p, h = ovl_hash(key);
        u32 it = 0;
        for (u16 *link = &ovl_head[h]; *link != OVL_NONE && it++ <= NHAL_OVL_PAGES;
             link = &ovl_next[*link]) {
            if (ovl_key[*link] == key) {
                u16 i = *link;
                *link = ovl_next[i];
                ovl_next[i] = ovl_free;
                ovl_free = i;
                ovl.used--;
                break;
            }
        }
    }
    ovl_erased[b / 32] |= 1u << (b % 32);
    ovl.erases++;
    s.buf_page = 0xFFFFFFFF;
}

/* ── page source ───────────────────────────────────────────────────── */

/* Disk page `lba` into data; false if outside the image or unreadable. */
static bool read_disk_page(u32 lba, u8 *data) {
    u32 sectors = nhal_disk_sectors();
    u32 chunk = lba / NHAL_SLOT_PAGES;
    u32 first = chunk * NHAL_SLOT_PAGES * 4u;
    if ((u64)lba * 4u + 4u > sectors)
        return false;

    u32 slot = chunk % NHAL_SLOTS;
    if (slot_tag[slot] != chunk + 1) {
        u32 count = sectors - first;
        if (count > NHAL_SLOT_PAGES * 4u)
            count = NHAL_SLOT_PAGES * 4u;
        stat_misses++;
        slot_tag[slot] = 0;
        if (!nhal_fill(slot, first, count)) {
            note_err(0xE3);
            static bool logged;
            if (!logged) {
                logged = true;
                nhal_log("[NAND] disk read failed at sector ", first);
            }
            return false;
        }
        slot_tag[slot] = chunk + 1;
    }
    copy(data, nhal_slot(slot) + (lba % NHAL_SLOT_PAGES) * NAND_PAGE_BYTES,
         NAND_PAGE_BYTES);
    return true;
}

void nand_get_page(u32 bank, u32 pn, u8 *data, u8 *spare) {
    u32 vpn = (pn / PAGES_PER_BLOCK) * PAGES_PER_SUBLOCK +
              (pn % PAGES_PER_BLOCK) * BANKS + bank;

    stat_pages++;

    /* Written since boot: what was programmed, or erased NAND. */
    u32 oi = ovl_find(bank, pn);
    if (oi != OVL_NONE) {
        copy(data, ovl_rec(oi), NAND_PAGE_BYTES);
        copy(spare, ovl_rec(oi) + NAND_PAGE_BYTES, NAND_SPARE_BYTES);
        return;
    }
    if (block_erased(bank, pn)) {
        for (u32 i = 0; i < NAND_PAGE_BYTES; i++)
            data[i] = 0xFF;
        for (u32 i = 0; i < NAND_SPARE_BYTES; i++)
            spare[i] = 0xFF;
        return;
    }

    zero(data, NAND_PAGE_BYTES);
    zero(spare, NAND_SPARE_BYTES);

    if (bank == 0 && pn == 0) {                 /* FIL ID */
        put32(data, 0, FIL_ID);
        return;
    }
    /* Bad block table, all good.  The generator puts it on page 0 of the
     * last block only, but on hardware the loader's reads of that block
     * reach us from page 1 up (0x7FF81...), so every page carries it. */
    if (pn >= BBT_PN && pn < BBT_PN + PAGES_PER_BLOCK) {
        /* The VFL copies `len` (+0x34) bitmap bytes from +0x38 into the map
         * _LoadVFLCxt consults; qemu's 1G generator leaves len 0, so the
         * map keeps stale memory.  Give every block a "good" bit. */
        copy(data, "DEVICEINFOBBT", 13);
        put32(data, 0x34, BLOCKS_PER_BANK / 8);
        for (u32 i = 0; i < BLOCKS_PER_BANK / 8; i++)
            data[0x38 + i] = 0xFF;
        return;
    }
    if (pn == VFL_CXT_PN) {                     /* VFL context, every bank */
        for (u32 i = 0; i < 281; i++)           /* aBadMark: no bad blocks */
            data[1672 + i] = 0xFF;
        put16(data, 1954, 35);                  /* awInfoBlk[0] */
        put32(spare, 0, 1);                     /* dwCxtAge */
        spare[9] = 0x80;                        /* VFL_CTX_SPARE_TYPE */
        return;
    }
    if (vpn == FTL_CXT_VPN) {                   /* first page of FTL cxt */
        spare[9] = 0x43;
        spare[10] = 0xFF;
        return;
    }
    if (vpn > FTL_CXT_VPN && vpn <= FTL_CXT_VPN + FTL_MAP_PAGES) {
        u32 t = vpn - FTL_CXT_VPN - 1;          /* LBN -> VBN map table t */
        for (u32 j = 0; j < 1024; j++)
            put16(data, j * 2, t * 1024 + j + 1);
        return;
    }
    if (vpn == FTL_META_VPN) {                  /* FTLMeta */
        put16(data, 8, 20);                     /* wNumOfFreeVb */
        for (u32 i = 0; i < 20; i++)
            put16(data, 14 + i * 2, 3 + i);     /* awFreeVbList */
        for (u32 i = 0; i < FTL_MAP_PAGES; i++)
            put32(data, 56 + i * 4, i + 1);     /* adwMapTablePtrs */
        for (u32 i = 0; i < 18; i++)
            put16(data, 420 + i * 20 + 4, 0xFFFF); /* aLOGCxtTable[].wVbn */
        put32(data, 2040, 0x46560000);          /* dwVersion */
        put32(data, 2044, 0xB9A9FFFF);          /* dwVersionNot */
        spare[9] = 0x43;
        return;
    }
    if (vpn >= NAND_DATA_VPN0 && read_disk_page(vpn - NAND_DATA_VPN0, data)) {
        spare[10] = 0xFF;                       /* ECC mark */
        return;
    }
    /* Unwritten pages.  In the FTL's data area (every logical block is
     * mapped by the generator's tables) they read as qemu-ios has them:
     * zeros with the ECC mark.  Erased NAND (all 0xFF) there stalls the FTL
     * for good: hardware hung on the first page past DISK.IMG (LBA 143378).
     * Below the data area, where the VFL searches for its context, they
     * read as erased NAND. */
    if (vpn >= NAND_DATA_VPN0) {
        zero(data, NAND_PAGE_BYTES);
        spare[10] = 0xFF;
        return;
    }
    for (u32 i = 0; i < NAND_PAGE_BYTES; i++)
        data[i] = 0xFF;
    for (u32 i = 0; i < NAND_SPARE_BYTES; i++)
        spare[i] = 0xFF;
}

/* ── FMC ───────────────────────────────────────────────────────────── */

static int get_bank(void) {
    u32 map = (s.fmctrl0 >> 1) & 0xFF;
    for (int b = 0; b < (int)BANKS; b++)
        if (map & (1u << b))
            return b;
    return -1;
}

static void set_bank(u32 bank) {
    s.fmctrl0 &= ~(0xFFu << 1);
    s.fmctrl0 |= 1u << (bank + 1);
}

static void buffer_page(u32 page) {
    int bank = get_bank();
    if (bank < 0) {
        static bool logged;
        if (!logged) {
            logged = true;
            nhal_log("[NAND] read with no bank selected, page ", page);
        }
        bank = 0;
    }
    if (bank != s.buf_bank || page != s.buf_page) {
        nand_get_page((u32)bank, page, page_buf, spare_buf);
        s.buf_bank = bank;
        s.buf_page = page;
    }
}

static u32 buf_word(const u8 *buf, u32 len, u32 idx) {
    return idx < len / 4 ? get32(buf + idx * 4) : 0;
}

/* After an ADM read the driver's PL080 drains the FIFO into the same
 * buffer the scatter list names, so the FIFO must hold exactly that
 * command's pages, whatever FMC registers it touches meanwhile: n x 2048
 * data bytes, then each page's 64 spare bytes. */
static u32 adm_fifo_word(void) {
    u32 pos = s.fifo_pos, data = s.fifo_n * NAND_PAGE_BYTES, v = 0xFFFFFFFF;
    if (pos < data) {
        u32 i = pos / NAND_PAGE_BYTES;
        set_bank(s.banks_to_read[i]);
        buffer_page(s.pages_to_read[i]);
        v = get32(page_buf + pos % NAND_PAGE_BYTES);
    } else if (pos < data + s.fifo_n * NAND_SPARE_BYTES) {
        u32 i = (pos - data) / NAND_SPARE_BYTES;
        set_bank(s.banks_to_read[i]);
        buffer_page(s.pages_to_read[i]);
        v = get32(spare_buf + (pos - data) % NAND_SPARE_BYTES);
    }
    s.fifo_pos += 4;
    return v;
}

/* FIFO as seen by the PL080: always the last ADM read's pages. */
u32 nand_fifo_read_dma(void) {
    return s.adm_fifo ? adm_fifo_word() : nand_fifo_read();
}

/* FIFO as seen by the CPU: ID/status answers win, then the ADM pages. */
u32 nand_fifo_read(void) {
    u32 v;
    if (s.adm_fifo && s.cmd != CMD_ID && s.cmd != CMD_READSTATUS)
        return adm_fifo_word();
    if (s.cmd == CMD_ID)
        return NAND_CHIP_ID;
    if (s.cmd == CMD_READSTATUS)
        return 1u << 6;

    if (s.reading_multi) {
        if (s.fmdnum % NAND_PAGE_BYTES == 0) {
            s.cur_bank_reading++;
            if (s.cur_bank_reading < (int)MULTI_MAX)
                set_bank(s.banks_to_read[s.cur_bank_reading]);
        }
        if (s.cur_bank_reading < 0 || s.cur_bank_reading >= (int)MULTI_MAX) {
            s.fmdnum -= 4;
            return 0;
        }
        u32 off = s.fmdnum % NAND_PAGE_BYTES;
        if (off == 0)
            off = NAND_PAGE_BYTES;
        buffer_page(s.pages_to_read[s.cur_bank_reading]);
        v = buf_word(page_buf, NAND_PAGE_BYTES, (NAND_PAGE_BYTES - off) / 4);
    } else {
        buffer_page((s.fmaddr1 << 16) | (s.fmaddr0 >> 16));
        if (s.reading_spare)
            v = buf_word(spare_buf, NAND_SPARE_BYTES,
                         (NAND_SPARE_BYTES - s.fmdnum - 1) / 4);
        else
            v = buf_word(page_buf, NAND_PAGE_BYTES,
                         (NAND_PAGE_BYTES - s.fmdnum - 1) / 4);
    }
    s.fmdnum -= 4;
    return v;
}

void nand_fifo_write(u32 val) {
    if (!s.is_writing)
        return;
    if (s.fmdnum >= 4 && s.fmdnum <= NAND_PAGE_BYTES)
        put32(page_buf, NAND_PAGE_BYTES - s.fmdnum, val);
    s.fmdnum -= 4;
    if (s.fmdnum == 0)
        s.is_writing = false;   /* not persisted, as in qemu-ios */
}

u32 nand_fmc_read(u32 off) {
    u32 v;
    switch (off) {
    case FMCTRL0: v = s.fmctrl0; break;
    case FMFIFO:  return nand_fifo_read();
    case FMCSTAT: v = 0x1FFE; break;    /* controller + all 8 banks ready */
    case RSCTRL:  v = s.rsctrl; break;
    default:      v = 0; break;
    }
    note_post_read(NAND_FMC_BASE + off, v);
    return v;
}

void nand_fmc_write(u32 off, u32 val) {
    switch (off) {
    case FMCTRL0: s.fmctrl0 = val; break;
    case FMCTRL1: s.fmctrl1 = val; break;
    case FMCMD:                         /* a PIO page read replaces it */
        s.cmd = val;
        if (val == CMD_READ)
            s.adm_fifo = false;
        break;
    case FMADDR0: s.fmaddr0 = val; break;
    case FMADDR1: s.fmaddr1 = val; break;
    case FMANUM:  s.fmanum = val; break;
    case FMDNUM:
        s.reading_spare = (val == NAND_SPARE_BYTES - 1);
        s.fmdnum = val;
        break;
    case FMFIFO:  nand_fifo_write(val); break;
    case RSCTRL:  s.rsctrl = val; break;
    default:      break;
    }
}

/* ── RS-ECC: every correction succeeds ─────────────────────────────── */

u32 nand_ecc_read(u32 off) {
    note_post_read(NAND_ECC_BASE + off, 0);
    return 0;
}

void nand_ecc_write(u32 off, u32 val) {
    (void)val;
    if (off == ECC_START)
        irq(NAND_IRQ_ECC, true);
    else if (off == ECC_CLEARINT)
        irq(NAND_IRQ_ECC, false);
}

/* ── ADM: interprets the FTL's command block ───────────────────────── */

/* Per-page metadata (first 12 spare bytes) to data section 3. qemu-ios
 * writes only the ECC mark; the VFL context check needs the real spare
 * (spare[8] == 0, spare[9] == 0x80) whichever read path it uses. */
static void spare_marks(u32 n) {
    static u8 d[NAND_PAGE_BYTES] ALIGNED(4), sp[NAND_SPARE_BYTES] ALIGNED(4);
    for (u32 i = 0; i < n && i < MULTI_MAX; i++) {
        nand_get_page(s.banks_to_read[i], s.pages_to_read[i], d, sp);
        nhal_phys_write(s.adm_data3 + i * 12, sp, 12);
    }
}

static void start_multi(u32 n) {
    s.reading_multi = true;
    s.fmdnum = n * NAND_PAGE_BYTES;
    s.cur_bank_reading = -1;
    spare_marks(n);
}

/* ── transport diagnostics ─────────────────────────────────────────── */

static struct {
    u32 cmds, reads, bytes, errs, last_err, post, dma, dma_bytes, writes, erases;
    bool after_cmd;
} st;

/* Last 16 ADM commands of any opcode, dumped once ADM goes quiet. */
#define RING 16
static struct {
    u32 seq, cmd, n, bank, page, nent, total, meta;
} ring[RING];
static u32 ring_seq, stall_seen_seq, stall_idle_since, stall_dumped_seq;
static u32 adm_acks;            /* CTRL2 writes without the start bit */
static u8  irq_level[64];       /* last level we drove per line */

static void irq(int line, bool level) {
    if (line >= 0 && line < 64)
        irq_level[line] = level;
    nhal_irq(line, level);
}



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

static void note_err(u32 code) {
    st.errs++;
    st.last_err = code;
}

static void stall_check(void);
u32 nand_dma_irq_level(int n);

void nand_status_line(char *buf) {
    /* "Nc<cmds> r<pages read> x<errs> s<last err> w<write cmds>
     *  e<erase cmds> o<overlay pages used> f<programs dropped, full>" */
    char *p = buf;
    /* No disk image is fatal and easy to miss in the scrolling log: once
     * the driver has issued a command, ask the block server (cached; the
     * answer is immediate unless the ARM9 never came up at all). */
    static u8 disk_state;               /* 0 unchecked, 1 ok, 2 missing */
    if (!disk_state && ring_seq)
        disk_state = nhal_disk_sectors() ? 1 : 2;
    if (disk_state == 2) {
        p = str(p, "N!! NO DISK.IMG: ARM9 block server has none !!");
        *p = 0;
        stall_check();
        return;
    }
    p = str(p, "Nc"); p = hx(p, st.cmds, 4);
    p = str(p, " r"); p = hx(p, st.reads, 5);
    p = str(p, " x"); p = hx(p, st.errs, 3);
    p = str(p, " s"); p = hx(p, st.last_err, 4);
    p = str(p, " w"); p = hx(p, st.writes, 4);
    p = str(p, " e"); p = hx(p, st.erases, 3);
    p = str(p, " o"); p = hx(p, ovl.used, 4);
    p = str(p, " f"); p = hx(p, ovl.full, 3);
    if (stall_dumped_seq && stall_dumped_seq == ring_seq)
        p = str(p, " S");               /* [STALL] dump printed, still idle */
    *p = 0;
    stall_check();
}

/* Ticks ADM has been quiet since its last command (0 before any). */
u32 nand_adm_idle(void) {
    if (ring_seq == 0 || ring_seq != stall_seen_seq)
        return 0;
    return nhal_ticks() - stall_idle_since;
}

/* Called with each status redraw: once ADM has been idle for ~2 s after
 * some activity, print the last commands (once per quiet period). */
static void stall_check(void) {
    u32 now = nhal_ticks();

    if (ring_seq != stall_seen_seq) {
        stall_seen_seq = ring_seq;
        stall_idle_since = now;
        return;
    }
    if (ring_seq == 0 || ring_seq == stall_dumped_seq || now - stall_idle_since < 200)
        return;
    stall_dumped_seq = ring_seq;
    char b[52], *p;
    p = b;
    p = str(p, "[STALL] ADM idle after #"); p = hx(p, ring_seq, 5);
    p = str(p, " acks "); p = hx(p, adm_acks, 5);

    *p = 0;
    nhal_puts(b);
    p = b;
    p = str(p, "[STALL] lines ADM"); p = hx(p, irq_level[NAND_IRQ_ADM], 1);
    p = str(p, " ECC"); p = hx(p, irq_level[NAND_IRQ_ECC], 1);
    p = str(p, " DMA0 "); p = hx(p, nand_dma_irq_level(0), 1);
    p = str(p, " DMA1 "); p = hx(p, nand_dma_irq_level(1), 1);
    *p = 0;
    nhal_puts(b);
    for (u32 k = 0; k < RING; k++) {
        u32 i = (ring_seq + k) % RING;
        if (!ring[i].seq)
            continue;
        p = b;
        p = hx(p, ring[i].seq, 4);
        p = str(p, " "); p = hx(p, ring[i].cmd, 4);
        p = str(p, " n"); p = hx(p, ring[i].n, 3);
        p = str(p, " b"); p = hx(p, ring[i].bank, 1);
        p = str(p, " p"); p = hx(p, ring[i].page, 5);
        p = str(p, " sg"); p = hx(p, ring[i].nent, 2);
        p = str(p, "/"); p = hx(p, ring[i].total, 5);
        p = str(p, " m"); p = hx(p, ring[i].meta, 2);
        *p = 0;
        nhal_puts(b);
    }
}

void nand_note_dma(u32 ch, u32 src, u32 dst, u32 bytes) {
    st.dma++;
    st.dma_bytes += bytes;
    if (st.dma <= 6) {
        char b[52], *p = b;
        p = str(p, "[DMA] c"); p = hx(p, ch, 1);
        p = str(p, " s"); p = hx(p, src, 8);
        p = str(p, " d"); p = hx(p, dst, 8);
        p = str(p, " n"); p = hx(p, bytes, 5);
        *p = 0;
        nhal_puts(b);
    }
}

static void note_post_read(u32 pa, u32 v) {
    if (!st.after_cmd)
        return;
    st.post++;
    if (st.post <= 8) {
        char b[52], *p = b;
        p = str(p, "[ADMr] "); p = hx(p, pa, 8);
        p = str(p, "="); p = hx(p, v, 8);
        *p = 0;
        nhal_puts(b);
    }
}

/* ── scatter-list delivery ─────────────────────────────────────────── */

/* Streams bytes into the command's {BE pa, BE len} list. */
static struct {
    u32 base, k, pa, len, off, total, nent;
    bool dead;
} sgs;

/* The driver doesn't terminate the list: entries past this command's
 * transfer can be stale ones from earlier, longer commands.  Only walk as
 * many entries as cover `need` bytes. */
static void sg_open(u32 need) {
    u32 sg = s.adm_data2 + ADM_SGLIST;
    sgs.base = sg;
    sgs.k = 0;
    sgs.off = 0;
    sgs.dead = false;
    sgs.total = sgs.nent = 0;
    for (u32 k = 0; k < ADM_SG_MAX && sgs.total < need; k++) {
        u32 l = phys_be32(sg + 8 * k + 4);
        if (l == 0)
            break;
        sgs.total += l;
        sgs.nent++;
    }
    sgs.pa = phys_be32(sg);
    sgs.len = sgs.nent ? phys_be32(sg + 4) : 0;
}

static void sg_put(const u8 *src, u32 n) {
    static bool shown;
    for (u32 it = 0; n && !sgs.dead; it++) {
        if (it > ADM_SG_MAX + 2) {
            hang_cap(&shown, "[NAND] cap put", sgs.k, sgs.nent, sgs.pa, n);
            sgs.dead = true;
            return;
        }
        if (sgs.off == sgs.len) {
            if (++sgs.k >= sgs.nent) {
                sgs.dead = true;
                return;
            }
            sgs.pa = phys_be32(sgs.base + 8 * sgs.k);
            sgs.len = phys_be32(sgs.base + 8 * sgs.k + 4);
            sgs.off = 0;
        }
        u32 c = n < sgs.len - sgs.off ? n : sgs.len - sgs.off;
        if (!nhal_phys_write(sgs.pa + sgs.off, src, c)) {
            note_err(0xE1);             /* scatter target outside RAM */
            sgs.dead = true;
            return;
        }
        st.bytes += c;
        sgs.off += c;
        src += c;
        n -= c;
    }
}

/* Pull n bytes out of the command's scatter list (program data). */
static void sg_get(u8 *dst, u32 n) {
    static bool shown;
    for (u32 it = 0; n && !sgs.dead; it++) {
        if (it > ADM_SG_MAX + 2) {
            hang_cap(&shown, "[NAND] cap get", sgs.k, sgs.nent, sgs.pa, n);
            sgs.dead = true;
            break;
        }
        if (sgs.off == sgs.len) {
            if (++sgs.k >= sgs.nent) {
                sgs.dead = true;
                break;
            }
            sgs.pa = phys_be32(sgs.base + 8 * sgs.k);
            sgs.len = phys_be32(sgs.base + 8 * sgs.k + 4);
            sgs.off = 0;
        }
        u32 c = n < sgs.len - sgs.off ? n : sgs.len - sgs.off;
        if (!nhal_phys_read(sgs.pa + sgs.off, dst, c)) {
            note_err(0xE5);             /* scatter source outside RAM */
            sgs.dead = true;
            break;
        }
        st.bytes += c;
        sgs.off += c;
        dst += c;
        n -= c;
    }
    while (n--)                         /* short list: pad as erased */
        *dst++ = 0xFF;
}

/* Program pages[0..n): n x 2048 data bytes from the scatter list, and
 * per page the first `meta` (+0x2C) spare bytes, which the driver copies
 * into data section 3 back to back (0xC04AC834), the same place reads
 * return them. */
static void sg_program(u32 n) {
    static u8 d[NAND_PAGE_BYTES] ALIGNED(4), m[NAND_SPARE_BYTES] ALIGNED(4);
    u32 meta = phys_be16(s.adm_data2 + ADM_META);
    if (meta > NAND_SPARE_BYTES)
        meta = NAND_SPARE_BYTES;
    sg_open(n * NAND_PAGE_BYTES);
    if (sgs.total < n * NAND_PAGE_BYTES)
        note_err(0xE2);
    for (u32 i = 0; i < n && i < MULTI_MAX; i++) {
        sg_get(d, NAND_PAGE_BYTES);
        if (meta)
            nhal_phys_read(s.adm_data3 + i * meta, m, meta);
        ovl_program(s.banks_to_read[i], s.pages_to_read[i], d, m, meta);
    }
}

/* Deliver pages[0..n) through the scatter list: n x 2048 data bytes, then
 * each page's first `meta` spare bytes (meta = command +0x2C: 12 for ECC
 * reads, 0 for raw).  Nothing is written past that. */
static void sg_deliver(u32 n) {
    u32 meta = phys_be16(s.adm_data2 + ADM_META);
    if (meta > NAND_SPARE_BYTES)
        meta = NAND_SPARE_BYTES;
    sg_open(n * (NAND_PAGE_BYTES + meta));
    if (sgs.nent == 0)
        return;
    CRUMB_IN(5);
    for (u32 i = 0; i < n && i < MULTI_MAX; i++) {
        set_bank(s.banks_to_read[i]);
        buffer_page(s.pages_to_read[i]);
        sg_put(page_buf, NAND_PAGE_BYTES);
    }
    for (u32 i = 0; meta && i < n && i < MULTI_MAX; i++) {
        set_bank(s.banks_to_read[i]);
        buffer_page(s.pages_to_read[i]);
        sg_put(spare_buf, meta);
    }
    if (sgs.total < n * NAND_PAGE_BYTES)
        note_err(0xE2);                 /* list shorter than the pages */
    CRUMB_OUT();
}

static void log_cmd(u32 cmd, u32 n) {
    st.cmds++;
    if (st.cmds > 8)
        return;
    char b[52], *p = b;
    p = str(p, "[ADM] op"); p = hx(p, cmd, 4);
    p = str(p, " n"); p = hx(p, n, 2);
    p = str(p, " b"); p = hx(p, s.banks_to_read[0], 1);
    p = str(p, " p"); p = hx(p, s.pages_to_read[0], 8);
    *p = 0;
    nhal_puts(b);
    p = b;
    p = str(p, "[ADM+] sg"); p = hx(p, sgs.nent, 2);
    p = str(p, " t"); p = hx(p, sgs.total, 6);
    p = str(p, " "); p = hx(p, phys_be32(s.adm_data2 + ADM_SGLIST), 8);
    p = str(p, "/"); p = hx(p, phys_be32(s.adm_data2 + ADM_SGLIST + 4), 5);
    p = str(p, sgs.dead ? " SHORT" : " ok");
    *p = 0;
    nhal_puts(b);
}

/* CE table (+0x34) as the last read/program command filled it: slot ->
 * chip enable.  Erases name the CE only. */
static u8 ce_of_slot[BANKS] = { 0, 1, 2, 3, 4, 5, 6, 7 };

static void note_ce_table(u32 blk) {
    for (u32 i = 0; i < BANKS; i++)
        ce_of_slot[i] = phys_u8(blk + ADM_CEMAP + i);
}

static u32 bank_of_ce(u32 ce) {
    for (u32 i = 0; i < BANKS; i++)
        if (ce_of_slot[i] == ce)
            return i;
    return ce & 7;
}

static void adm_command(void) {
    u32 blk = s.adm_data2;
    u32 cmd = phys_le32(blk + ADM_CMD) & 0xFFFF;
    u32 n = phys_be16(blk + ADM_NPAGES);
    sgs.nent = sgs.total = 0;
    sgs.dead = false;
    st.after_cmd = false;
    s.adm_fifo = false;                 /* only a read re-arms it */

    if (cmd >= 0x200 && cmd <= 0x500)
        note_ce_table(blk);
    switch (cmd) {
    case 0x200: {               /* n pages striped over the 8 banks */
        /* The driver (0xC04AC774) writes one base page per bank slot:
         * bank +0x44+i, page +0x244+4i.  Page j of the transfer is slot
         * j % 8, base + j / 8 (qemu-ios assumed slot 0's page for all). */
        u32 base[BANKS], bank[BANKS];
        if (n > MULTI_MAX)
            n = MULTI_MAX;
        for (u32 b = 0; b < BANKS; b++) {
            base[b] = phys_be32(blk + ADM_PAGES + 4 * b);
            bank[b] = phys_u8(blk + ADM_BANKS + b) & 7;
        }
        for (u32 j = 0; j < n; j++) {
            s.pages_to_read[j] = base[j % BANKS] + j / BANKS;
            s.banks_to_read[j] = bank[j % BANKS];
        }
        start_multi(n);
        sg_deliver(n);
        s.cur_bank_reading = -1;
        break;
    }
    case 0x300:
        if (n == 1) {           /* one page, spare copied to data section 3 */
            u32 bank = phys_u8(blk + ADM_BANKS) & 7;
            u32 page = phys_be32(blk + ADM_PAGES);
            s.reading_multi = false;
            set_bank(bank);
            nand_fmc_write(FMDNUM, NAND_PAGE_BYTES - 1);
            nand_fmc_write(FMADDR0, page << 16);
            nand_fmc_write(FMADDR1, (page >> 16) & 0xFF);
            nand_fmc_write(FMCMD, CMD_READ);
            buffer_page(page);
            nhal_phys_write(s.adm_data3, spare_buf, NAND_SPARE_BYTES);
            s.pages_to_read[0] = page;
            s.banks_to_read[0] = bank;
            sg_deliver(1);
            set_bank(bank);
        } else if (n > 1) {     /* scattered pages */
            if (n > MULTI_MAX)
                n = MULTI_MAX;
            for (u32 i = 0; i < n; i++) {
                s.pages_to_read[i] = phys_be32(blk + ADM_PAGES + 4 * i);
                s.banks_to_read[i] = phys_u8(blk + ADM_BANKS + i) & 7;
            }
            start_multi(n);
            sg_deliver(n);
            s.cur_bank_reading = -1;
        }
        break;
    case 0x400: {               /* program n pages striped like 0x200 */
        u32 base[BANKS], bank[BANKS];
        if (n > MULTI_MAX)
            n = MULTI_MAX;
        for (u32 b = 0; b < BANKS; b++) {
            base[b] = phys_be32(blk + ADM_PAGES + 4 * b);
            bank[b] = phys_u8(blk + ADM_BANKS + b) & 7;
        }
        for (u32 j = 0; j < n; j++) {
            s.pages_to_read[j] = base[j % BANKS] + j / BANKS;
            s.banks_to_read[j] = bank[j % BANKS];
        }
        sg_program(n);
        break;
    }
    case 0x500:                 /* program n scattered pages, like 0x300 */
        if (n > MULTI_MAX)
            n = MULTI_MAX;
        for (u32 i = 0; i < n; i++) {
            s.pages_to_read[i] = phys_be32(blk + ADM_PAGES + 4 * i);
            s.banks_to_read[i] = phys_u8(blk + ADM_BANKS + i) & 7;
        }
        sg_program(n);
        break;
    case 0x600: {               /* erase one block (slot 0 = its CE) */
        /* The FIL's erase (0xC046D278) passes block << 7, the block's
         * first page, and the builder (0xC04AC11C) stores the chip enable
         * itself in the CE table at +0x34 with slot 0 at +0x44 (reads and
         * programs store a slot per page and the table holds every CE;
         * reads and programs address pages by slot, so map the CE back to
         * the slot that the last read/program's table gave it). */
        u32 ce = phys_u8(blk + ADM_CEMAP + (phys_u8(blk + ADM_BANKS) & 7));
        u32 page = phys_be32(blk + ADM_PAGES);
        ovl_erase(bank_of_ce(ce), page);
        if (ovl.erases <= 4) {
            char b[52], *p = b;
            p = str(p, "[NAND] erase ce"); p = hx(p, ce, 1);
            p = str(p, " page "); p = hx(p, page, 5);
            p = str(p, " n"); p = hx(p, n, 2);
            *p = 0;
            nhal_puts(b);
        }
        break;
    }
    case 0x100:                 /* stale firmware bytes after the load */
        break;
    default: {
        static u32 logged;
        note_err(cmd);
        if (logged++ < 4)
            nhal_log("[NAND] unknown ADM command ", cmd);
        break;
    }
    }
    if (cmd == 0x400 || cmd == 0x500)
        st.writes++;
    else if (cmd == 0x600)
        st.erases++;
    {
        u32 i = ring_seq++ % RING;
        ring[i].seq = ring_seq;
        ring[i].cmd = cmd;
        ring[i].n = n;
        ring[i].bank = phys_u8(blk + ADM_BANKS) & 7;
        ring[i].page = phys_be32(blk + ADM_PAGES);
        ring[i].nent = sgs.nent;
        ring[i].total = sgs.total;
        ring[i].meta = phys_be16(blk + ADM_META);
    }
    if ((cmd == 0x200 || cmd == 0x300) && n) {
        st.reads += n;
        log_cmd(cmd, n);
        if (cmd == 0x300 && n == 1) {       /* single page: list is page 0 */
            s.pages_to_read[0] = phys_be32(blk + ADM_PAGES);
            s.banks_to_read[0] = phys_u8(blk + ADM_BANKS) & 7;
        }
        s.fifo_n = n > MULTI_MAX ? MULTI_MAX : n;
        s.fifo_pos = 0;
        s.adm_fifo = true;
    }
    st.after_cmd = true;
}

u32 nand_adm_read(u32 off) {
    u32 v;
    switch (off) {
    case ADM_CTRL:  v = 0x2; break;     /* ready */
    case ADM_CTRL2: v = 0x10; break;    /* command finished */
    default:        v = 0; break;
    }
    note_post_read(NAND_ADM_BASE + off, v);
    return v;
}

void nand_adm_write(u32 off, u32 val) {
    switch (off) {
    case ADM_CTRL:
        if (val == 3) {             /* start-up: report the 8 banks */
            u8 id[32];
            put32(id, 0, 0x50);
            nhal_phys_write(s.adm_data2, id, 4);
            for (u32 i = 0; i < BANKS; i++)
                put32(id, i * 4, NAND_CHIP_ID);
            nhal_phys_write(s.adm_data3, id, 32);
        }
        break;
    case ADM_CTRL2:
        if (val == 2) {
            adm_command();
            irq(NAND_IRQ_ADM, true);
        }
        if ((val & 2) == 0) {
            adm_acks++;
            irq(NAND_IRQ_ADM, false);
        }
        break;
    case ADM_CODE_SEC:  s.adm_code = val; break;
    case ADM_DATA1_SEC: s.adm_data1 = val; break;
    case ADM_DATA2_SEC: s.adm_data2 = val; break;
    case ADM_DATA3_SEC: s.adm_data3 = val; break;
    default: break;
    }
}

/* ── init / stats ──────────────────────────────────────────────────── */

void nand_init(void) {
    zero(&s, sizeof(s));
    s.buf_bank = -1;
    s.buf_page = 0xFFFFFFFF;
    ovl_init();
    for (u32 i = 0; i < NHAL_SLOTS; i++)
        slot_tag[i] = 0;
}

u32 nand_pages_read(void)   { return stat_pages; }
u32 nand_cache_misses(void) { return stat_misses; }
