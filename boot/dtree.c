/*
 * dtree.c — Apple Device Tree for iOS 3DS bootloader
 *
 * Copies Apple's real n45ap tree (embedded by dtblob.S) into the kernel's
 * device-tree buffer, keeping only the nodes our peripheral emulation can
 * back, then fills in the placeholders iBoot would normally populate.
 */

#include "dtree.h"
#include "bootargs.h"
#include "console.h"

extern const u8 n45ap_dt_start[];
extern const u8 n45ap_dt_end[];

/* Nodes (besides the root) handed to the kernel.  Everything else would
 * start a driver for hardware we don't emulate (NAND, LCD, USB, audio,
 * sensors, ...); a parent that isn't listed drops its whole subtree. */
static const char *const dt_keep[] = {
    "chosen",
    "chosen/memory-map",
    "chosen/iBoot",
    "options",
    "cpus",
    "cpus/cpu0",
    "memory",
    "vram",                         /* CoreSurface "PurpleGfxMem" region */
    "buttons",                      /* 'buttons,n45': GPIO 0x2D hold, 0x2E menu */
    "button-wake",                  /* AppleM68WakeButton: PMU int 10 (EXTON1)
                                     * + STAT, which button woke the system */
    "charger",                      /* ApplePCF50635PMUPowerSource::start
                                     * uses fromPath("/charger") unchecked */
    "arm-io",
    "arm-io/vic",
    "arm-io/gpio",
    "arm-io/power",
    "arm-io/clkrstgen",
    "arm-io/sha1",
    "arm-io/aes",
    "arm-io/spi0",
    "arm-io/spi0/lcd0",             /* LM2512 MPL serializer (AppleMerlotLCD) */
    "arm-io/spi1",
    "arm-io/spi1/lcd0",             /* Merlot panel (AppleMerlotLCD) */
    "arm-io/dmac1",                 /* PL080: SPI2's dma-parent; without it the
                                     * SPI controller's IODMAEventSource
                                     * waits forever (start 0xC049529C) */
    "arm-io/spi2",
    "arm-io/spi2/multi-touch",      /* Zephyr 'multi-touch,z2' (periph/input.c) */
    "arm-io/clcd",                  /* AppleH1CLCD -> IOMobileFramebuffer "LCD" */
    "arm-io/i2c0",
    "arm-io/i2c1",
    "arm-io/i2c1/pmu",
    "arm-io/timer",
    "arm-io/uart0",
    "arm-io/chipid",
#if ROOT_NAND
    /* NAND root: PL080 used by the FMC, the ADM command processor, the FMC
     * and RS-ECC (both named flash-controller0) and the FTL's disk node. */
    "arm-io/dmac0",
    "arm-io/adm",
    "arm-io/flash-controller0",
    "arm-io/flash-controller0/disk",
#endif
};

#define PROP_NAME_LEN 32

/* ── Raw tree walking ───────────────────────────────────────────────── */

static u32 rd32(const u8 *p) { return *(const u32 *)p; }

static u32 str_len(const char *s) {
    u32 n = 0;
    while (s[n]) n++;
    return n;
}

static bool str_eq(const char *a, const char *b) {
    while (*a && *a == *b) { a++; b++; }
    return *a == *b;
}

static u32 prop_len(const u8 *p) { return rd32(p + PROP_NAME_LEN) & 0x7FFFFFFF; }

static const u8 *prop_next(const u8 *p) {
    return p + PROP_NAME_LEN + 4 + ((prop_len(p) + 3) & ~3u);
}

static const u8 *props_end(const u8 *node) {
    const u8 *p = node + 8;
    for (u32 i = 0; i < rd32(node); i++)
        p = prop_next(p);
    return p;
}

static u8 *find_prop(u8 *node, const char *name) {
    u8 *p = node + 8;
    for (u32 i = 0; i < rd32(node); i++) {
        if (str_eq((const char *)p, name))
            return p;
        p = (u8 *)prop_next(p);
    }
    return NULL;
}

static const char *node_name(const u8 *node) {
    u8 *p = find_prop((u8 *)node, "name");
    return p ? (const char *)(p + PROP_NAME_LEN + 4) : "";
}

static const u8 *node_end(const u8 *node) {
    const u8 *c = props_end(node);
    for (u32 i = 0; i < rd32(node + 4); i++)
        c = node_end(c);
    return c;
}

/* ── Filtered copy ──────────────────────────────────────────────────── */

static u8  *out;
static u32  out_pos, out_max;
static bool out_overflow;

static void out_bytes(const u8 *src, u32 n) {
    if (out_pos + n > out_max) {
        out_overflow = true;
        return;
    }
    for (u32 i = 0; i < n; i++)
        out[out_pos + i] = src[i];
    out_pos += n;
}

static bool path_kept(const char *path) {
    for (u32 i = 0; i < sizeof(dt_keep) / sizeof(dt_keep[0]); i++)
        if (str_eq(dt_keep[i], path))
            return true;
    return false;
}

#define PATH_MAX_LEN 128

static const u8 *copy_node(const u8 *node, char *path, u32 plen, bool keep) {
    const u8 *children = props_end(node);
    u32 hdr = out_pos;
    if (keep)
        out_bytes(node, (u32)(children - node));

    u32 kept = 0;
    const u8 *c = children;
    for (u32 i = 0; i < rd32(node + 4); i++) {
        const char *name = node_name(c);
        u32 l = plen;
        if (l && l + 1 < PATH_MAX_LEN)
            path[l++] = '/';
        for (u32 j = 0; name[j] && l + 1 < PATH_MAX_LEN; j++)
            path[l++] = name[j];
        path[l] = 0;

        bool k = keep && path_kept(path);
        c = copy_node(c, path, l, k);
        path[plen] = 0;
        if (k)
            kept++;
    }

    if (keep && !out_overflow)
        *(u32 *)(out + hdr + 4) = kept;
    return c;
}

/* ── Placeholder filling (on the copied tree) ───────────────────────── */

static u8 *find_child(u8 *node, const char *name, u32 name_len) {
    u8 *c = (u8 *)props_end(node);
    for (u32 i = 0; i < rd32(node + 4); i++) {
        const char *n = node_name(c);
        if (str_len(n) == name_len) {
            u32 j = 0;
            while (j < name_len && n[j] == name[j]) j++;
            if (j == name_len)
                return c;
        }
        c = (u8 *)node_end(c);
    }
    return NULL;
}

static u8 *find_node(const char *path) {
    u8 *node = out;
    while (node && *path) {
        u32 l = 0;
        while (path[l] && path[l] != '/') l++;
        node = find_child(node, path, l);
        path += l;
        if (*path == '/') path++;
    }
    return node;
}

static void dt_fail(const char *path, const char *name) {
    console_print("[DTREE] missing/short placeholder ");
    console_print(path);
    console_print(":");
    console_print(name);
    console_print("\n");
}

static void set_prop(const char *path, const char *name, const void *val, u32 len) {
    u8 *node = find_node(path);
    u8 *p = node ? find_prop(node, name) : NULL;
    if (!p || prop_len(p) < len) {
        dt_fail(path, name);
        return;
    }
    u8 *v = p + PROP_NAME_LEN + 4;
    for (u32 i = 0; i < len; i++)
        v[i] = ((const u8 *)val)[i];
}

static void set_u32(const char *path, const char *name, u32 val) {
    set_prop(path, name, &val, 4);
}

static void rename_prop(u8 *p, const char *name) {
    u32 l = str_len(name);
    for (u32 i = 0; i < PROP_NAME_LEN; i++)
        p[i] = i < l ? (u8)name[i] : 0;
}

static u32 mm_next;   /* next free MemoryMapReserved-N slot */

static void mm_add(const char *name, u32 addr, u32 size) {
    static const char prefix[] = "MemoryMapReserved-";
    char slot[24];
    u32 l = 0;
    while (prefix[l]) {
        slot[l] = prefix[l];
        l++;
    }
    if (mm_next >= 10)
        slot[l++] = (char)('0' + mm_next / 10);
    slot[l++] = (char)('0' + mm_next % 10);
    slot[l] = 0;
    mm_next++;

    u8 *mm = find_node("chosen/memory-map");
    u8 *p = mm ? find_prop(mm, slot) : NULL;
    if (!p || prop_len(p) < 8) {
        dt_fail("chosen/memory-map", slot);
        return;
    }
    rename_prop(p, name);
    *(u32 *)(p + PROP_NAME_LEN + 4) = addr;
    *(u32 *)(p + PROP_NAME_LEN + 8) = size;
}

/* ── Public ─────────────────────────────────────────────────────────── */

size_t dtree_build_n45ap(void *buffer, size_t max_size, u32 mem_size,
                         u32 ramdisk_addr, u32 ramdisk_size)
{
    char path[PATH_MAX_LEN];
    path[0] = 0;

    out = (u8 *)buffer;
    out_pos = 0;
    out_max = (u32)max_size;
    out_overflow = false;
    mm_next = 0;

    const u8 *end = copy_node(n45ap_dt_start, path, 0, true);
    if (out_overflow || end != n45ap_dt_end) {
        console_print("[DTREE] ERROR: bad embedded tree or buffer too small\n");
        return 0;
    }

    /* cpu0: the S5L timer divider is fixed/timebase, so n=1 and the
     * timer and our counter tick at 24 MHz. */
    set_u32("cpus/cpu0", "timebase-frequency",   24000000);
    set_u32("cpus/cpu0", "fixed-frequency",      24000000);
    set_u32("cpus/cpu0", "peripheral-frequency", 24000000);
    set_u32("cpus/cpu0", "clock-frequency",      268111856);
    set_u32("cpus/cpu0", "bus-frequency",        134055928);
    set_u32("cpus/cpu0", "memory-frequency",     134055928);

    {
        u32 reg[2] = { 0x08000000, mem_size };
        set_prop("memory", "reg", reg, sizeof(reg));
    }

    set_u32("chosen", "debug-enabled", 1);

    /* Graphics memory, below physBase so the VM never owns it: the boot
     * framebuffer sits at its start, as iBoot lays it out; AppleH1CLCD and
     * IOCoreSurfaceRoot carve the LCD framebuffer and surfaces from it. */
    {
        u32 reg[2] = { DT_VRAM_PA, DT_VRAM_SIZE };
        set_prop("vram", "reg", reg, sizeof(reg));
    }
    /* iBoot reads the panel ID over SPI1 (commands 0xDA/0xDB/0xDC); our
     * panel model answers 0x71, 0xC2, 0x00 like qemu-ios's. */
    set_u32("arm-io/spi1/lcd0", "lcd-panel-id", 0x0071C200);

    /* iBoot renames MemoryMapReserved-N slots to the loaded images.
     * Physical addresses are real FCRAM (physBase 0x21000000). */
    mm_add("Kernel-__TEXT",     0x21008000, 0x0019D000);
    mm_add("Kernel-__DATA",     0x211A5000, 0x000BF000);
    mm_add("Kernel-__HIB",      0x21000000, 0x00005000);
    mm_add("Kernel-__KLD",      0x21264000, 0x00037000);
    mm_add("Kernel-__PRELINK",  0x2129B000, 0x00366000);
    mm_add("Kernel-__LINKEDIT", 0x21601000, 0x00062520);
    mm_add("Kernel-__HEADER",   0x21008000, 0x00001000);
    mm_add("Kernel-__SYMTAB",   0x21601000, 0x00062520);
    mm_add("BootArgs",          0x27FA0000, 0x00001000);
    if (ramdisk_size > 0)
        mm_add("RAMDisk", ramdisk_addr, ramdisk_size);

    /* Our tree buffer lies in memory the kernel already treats as free,
     * so a "DeviceTree" entry would let it free those pages twice. */
    {
        u8 *mm = find_node("chosen/memory-map");
        u8 *p = mm ? find_prop(mm, "DeviceTree") : NULL;
        if (p)
            rename_prop(p, "MemoryMapReserved-16");
    }

    console_print("[DTREE] n45ap tree: ");
    console_print_hex(out_pos);
    console_print(" bytes\n");
    return out_pos;
}
