/*
 * S5L8900 SHA-1 engine (PA 0x38000000) as AppleS5L8900XSHA1 drives it
 * (prelinked kext, operation 0xC04A2CEC / quantum 0xC04A2BEC / result
 * 0xC04A2944).  The driver pads in software, so the engine only runs the
 * SHA-1 compression function over whole 64-byte blocks:
 *
 *   +0x00 config: bit 1 = go (0xA polled, 0xE with completion interrupt);
 *                 bit 0 reads as busy
 *   +0x04 reset (pulse): reload the initial state
 *   +0x08 / +0x0C  enable / chunk flag (stored)
 *   +0x20..+0x30   H0..H4 as 32-bit values (the driver byte-swaps them)
 *   +0x40..+0x7C   16-word block buffer (non-memory mode, qemu-ios)
 *   +0x80 memory mode   +0x84 source PA   +0x8C length in bytes
 *
 * Interrupt: device tree sha1 "interrupts" = 0x28 (VIC1 line 8).
 */

#include "sha1.h"
#include "vic.h"
#include "../iowin.h"
#include "../console.h"

#define SHA_CONFIG      0x00
#define SHA_RESET       0x04
#define SHA_HASH        0x20
#define SHA_BUF         0x40
#define SHA_MEM_MODE    0x80
#define SHA_MEM_START   0x84
#define SHA_INSIZE      0x8C
#define SHA_IRQ_LINE    0x28

static u32 regs[0x100 / 4];
static u32 h[5];
static u32 buf[16];
static bool buf_dirty;

static u32 rol(u32 x, u32 n) {
    return (x << n) | (x >> (32 - n));
}

static void sha1_block(const u8 *p) {
    u32 w[80];
    for (u32 i = 0; i < 16; i++)
        w[i] = (u32)p[4 * i] << 24 | (u32)p[4 * i + 1] << 16 |
               (u32)p[4 * i + 2] << 8 | p[4 * i + 3];
    for (u32 i = 16; i < 80; i++)
        w[i] = rol(w[i - 3] ^ w[i - 8] ^ w[i - 14] ^ w[i - 16], 1);
    u32 a = h[0], b = h[1], c = h[2], d = h[3], e = h[4];
    for (u32 i = 0; i < 80; i++) {
        u32 f, k;
        if (i < 20)      { f = (b & c) | (~b & d);          k = 0x5A827999; }
        else if (i < 40) { f = b ^ c ^ d;                   k = 0x6ED9EBA1; }
        else if (i < 60) { f = (b & c) | (b & d) | (c & d); k = 0x8F1BBCDC; }
        else             { f = b ^ c ^ d;                   k = 0xCA62C1D6; }
        u32 t = rol(a, 5) + f + e + k + w[i];
        e = d; d = c; c = rol(b, 30); b = a; a = t;
    }
    h[0] += a; h[1] += b; h[2] += c; h[3] += d; h[4] += e;
}

static void sha1_reset(void) {
    h[0] = 0x67452301; h[1] = 0xEFCDAB89; h[2] = 0x98BADCFE;
    h[3] = 0x10325476; h[4] = 0xC3D2E1F0;
    for (u32 i = 0; i < 16; i++)
        buf[i] = 0;
    buf_dirty = false;
}

static void sha1_go(u32 cfg) {
    if (buf_dirty) {                    /* CPU-written block */
        sha1_block((const u8 *)buf);
        for (u32 i = 0; i < 16; i++)
            buf[i] = 0;
        buf_dirty = false;
    }
    if (regs[SHA_MEM_MODE / 4]) {       /* DMA from memory */
        u32 pa = regs[SHA_MEM_START / 4], len = regs[SHA_INSIZE / 4] & ~63u;
        if (pa - 0x21000000u < 0x07000000u &&
            pa + len - 0x21000000u <= 0x07000000u) {
            const u8 *p = (const u8 *)io_va(pa);
            for (u32 off = 0; off < len; off += 64)
                sha1_block(p + off);
        } else {
            static bool shown;
            if (!shown) {
                shown = true;
                console_print("[SHA] bad DMA pa=");
                console_print_hex(pa);
                console_print(" len=");
                console_print_hex(len);
                console_print("\n");
            }
        }
    }
    if (cfg & 4)
        vic_set_irq(SHA_IRQ_LINE, true);
}

void sha1_init(void) {
    sha1_reset();
}

u32 sha1_read(u32 off) {
    if (off >= SHA_HASH && off < SHA_HASH + 20)
        return h[(off - SHA_HASH) / 4];
    if (off == SHA_CONFIG)
        return regs[0] & ~1u;           /* never busy */
    return off < sizeof(regs) ? regs[off / 4] : 0;
}

void sha1_write(u32 off, u32 value) {
    if (off >= sizeof(regs))
        return;
    if (off >= SHA_BUF && off < SHA_BUF + 0x40) {
        buf[(off - SHA_BUF) / 4] = value;
        buf_dirty = true;
        return;
    }
    switch (off) {
    case SHA_CONFIG:
        vic_set_irq(SHA_IRQ_LINE, false);
        regs[0] = value;
        if (value & 2)
            sha1_go(value);
        return;
    case SHA_RESET:
        if (value)
            sha1_reset();
        break;
    }
    regs[off / 4] = value;
}
