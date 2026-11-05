/*
 * S5L8900 AES engine (PA 0x38C00000), as the kernel's AES driver uses it
 * (prelinked kext, setup at VA 0xC04A1B6C, operation at 0xC04A1CB4):
 *
 *   +0x00 control (1 = enable)        +0x04 GO (1 = start)
 *   +0x08 reset pulse (1 then 0)      +0x0C status: bit 0 = done (polled)
 *   +0x10 0/7                         +0x14 mode: bit 0 = encrypt (15 enc,
 *                                           14 dec), bits 5:4 = key size
 *                                           (0 = 128, 1 = 192, 2 = 256)
 *   +0x18 length                      +0x20 / +0x24 input PA / segment len
 *   +0x28 / +0x2C output PA / length (mirrored to +0x30 / +0x34)
 *   +0x4C..+0x68 custom key, 8 big-endian words, key in the last words
 *   +0x6C key select: 0 custom, 1 GID, 2 UID
 *   +0x74..+0x80 IV, 4 big-endian words
 *
 * Operations complete synchronously on GO (status = 0xF, as qemu-ios).
 * GID: the S5L8900 GID key is not known; the GID-derived Key 0x837 stands
 * in for it.  UID: qemu-ios's fixed device key.
 */

#include "aes.h"
#include "../iowin.h"
#include "../console.h"

#define AES_CONTROL 0x00
#define AES_GO      0x04
#define AES_RESET   0x08
#define AES_STATUS  0x0C
#define AES_MODE    0x14
#define AES_LEN     0x18
#define AES_INADDR  0x20
#define AES_OUTADDR 0x28
#define AES_KEY     0x4C
#define AES_KEYSEL  0x6C
#define AES_IV      0x74

static u32 regs[0x100 / 4];

/* ── AES core (FIPS-197), byte oriented ─────────────────────────────── */

static const u8 sbox[256] = {
    0x63,0x7c,0x77,0x7b,0xf2,0x6b,0x6f,0xc5,0x30,0x01,0x67,0x2b,0xfe,0xd7,0xab,0x76,
    0xca,0x82,0xc9,0x7d,0xfa,0x59,0x47,0xf0,0xad,0xd4,0xa2,0xaf,0x9c,0xa4,0x72,0xc0,
    0xb7,0xfd,0x93,0x26,0x36,0x3f,0xf7,0xcc,0x34,0xa5,0xe5,0xf1,0x71,0xd8,0x31,0x15,
    0x04,0xc7,0x23,0xc3,0x18,0x96,0x05,0x9a,0x07,0x12,0x80,0xe2,0xeb,0x27,0xb2,0x75,
    0x09,0x83,0x2c,0x1a,0x1b,0x6e,0x5a,0xa0,0x52,0x3b,0xd6,0xb3,0x29,0xe3,0x2f,0x84,
    0x53,0xd1,0x00,0xed,0x20,0xfc,0xb1,0x5b,0x6a,0xcb,0xbe,0x39,0x4a,0x4c,0x58,0xcf,
    0xd0,0xef,0xaa,0xfb,0x43,0x4d,0x33,0x85,0x45,0xf9,0x02,0x7f,0x50,0x3c,0x9f,0xa8,
    0x51,0xa3,0x40,0x8f,0x92,0x9d,0x38,0xf5,0xbc,0xb6,0xda,0x21,0x10,0xff,0xf3,0xd2,
    0xcd,0x0c,0x13,0xec,0x5f,0x97,0x44,0x17,0xc4,0xa7,0x7e,0x3d,0x64,0x5d,0x19,0x73,
    0x60,0x81,0x4f,0xdc,0x22,0x2a,0x90,0x88,0x46,0xee,0xb8,0x14,0xde,0x5e,0x0b,0xdb,
    0xe0,0x32,0x3a,0x0a,0x49,0x06,0x24,0x5c,0xc2,0xd3,0xac,0x62,0x91,0x95,0xe4,0x79,
    0xe7,0xc8,0x37,0x6d,0x8d,0xd5,0x4e,0xa9,0x6c,0x56,0xf4,0xea,0x65,0x7a,0xae,0x08,
    0xba,0x78,0x25,0x2e,0x1c,0xa6,0xb4,0xc6,0xe8,0xdd,0x74,0x1f,0x4b,0xbd,0x8b,0x8a,
    0x70,0x3e,0xb5,0x66,0x48,0x03,0xf6,0x0e,0x61,0x35,0x57,0xb9,0x86,0xc1,0x1d,0x9e,
    0xe1,0xf8,0x98,0x11,0x69,0xd9,0x8e,0x94,0x9b,0x1e,0x87,0xe9,0xce,0x55,0x28,0xdf,
    0x8c,0xa1,0x89,0x0d,0xbf,0xe6,0x42,0x68,0x41,0x99,0x2d,0x0f,0xb0,0x54,0xbb,0x16,
};

static u8 inv_sbox[256];

static u8 xtime(u8 x) {
    return (u8)((x << 1) ^ ((x >> 7) * 0x1b));
}

static u8 gmul(u8 a, u8 b) {
    u8 p = 0;
    while (b) {
        if (b & 1)
            p ^= a;
        a = xtime(a);
        b >>= 1;
    }
    return p;
}

typedef struct {
    u32 nr;
    u8  rk[240];
} aes_ctx_t;

static void aes_expand(aes_ctx_t *c, const u8 *key, u32 keybits) {
    u32 nk = keybits / 32;
    c->nr = nk + 6;
    u32 total = 4 * (c->nr + 1);
    for (u32 i = 0; i < nk * 4; i++)
        c->rk[i] = key[i];
    u8 rcon = 1;
    for (u32 i = nk; i < total; i++) {
        u8 t[4];
        for (u32 j = 0; j < 4; j++)
            t[j] = c->rk[(i - 1) * 4 + j];
        if (i % nk == 0) {
            u8 r = t[0];
            t[0] = sbox[t[1]] ^ rcon;
            t[1] = sbox[t[2]];
            t[2] = sbox[t[3]];
            t[3] = sbox[r];
            rcon = xtime(rcon);
        } else if (nk > 6 && i % nk == 4) {
            for (u32 j = 0; j < 4; j++)
                t[j] = sbox[t[j]];
        }
        for (u32 j = 0; j < 4; j++)
            c->rk[i * 4 + j] = c->rk[(i - nk) * 4 + j] ^ t[j];
    }
}

static void add_round_key(u8 *s, const u8 *rk) {
    for (u32 i = 0; i < 16; i++)
        s[i] ^= rk[i];
}

/* State is column-major: s[4*c + r]. */
static void aes_encrypt_block(const aes_ctx_t *c, u8 *s) {
    add_round_key(s, c->rk);
    for (u32 round = 1; round <= c->nr; round++) {
        u8 t[16];
        for (u32 i = 0; i < 16; i++)            /* SubBytes + ShiftRows */
            t[i] = sbox[s[(i + 4 * (i % 4)) % 16]];
        if (round != c->nr) {                    /* MixColumns */
            for (u32 col = 0; col < 4; col++) {
                u8 *a = &t[4 * col];
                u8 a0 = a[0], a1 = a[1], a2 = a[2], a3 = a[3];
                u8 x = a0 ^ a1 ^ a2 ^ a3;
                a[0] ^= x ^ xtime(a0 ^ a1);
                a[1] ^= x ^ xtime(a1 ^ a2);
                a[2] ^= x ^ xtime(a2 ^ a3);
                a[3] ^= x ^ xtime(a3 ^ a0);
            }
        }
        for (u32 i = 0; i < 16; i++)
            s[i] = t[i];
        add_round_key(s, c->rk + 16 * round);
    }
}

static void aes_decrypt_block(const aes_ctx_t *c, u8 *s) {
    add_round_key(s, c->rk + 16 * c->nr);
    for (u32 round = c->nr; round-- > 0; ) {
        u8 t[16];
        for (u32 i = 0; i < 16; i++)            /* InvShiftRows + InvSubBytes */
            t[(i + 4 * (i % 4)) % 16] = inv_sbox[s[i]];
        for (u32 i = 0; i < 16; i++)
            s[i] = t[i];
        add_round_key(s, c->rk + 16 * round);
        if (round != 0) {                        /* InvMixColumns */
            for (u32 col = 0; col < 4; col++) {
                u8 *a = &s[4 * col];
                u8 a0 = a[0], a1 = a[1], a2 = a[2], a3 = a[3];
                a[0] = gmul(a0, 14) ^ gmul(a1, 11) ^ gmul(a2, 13) ^ gmul(a3, 9);
                a[1] = gmul(a0, 9) ^ gmul(a1, 14) ^ gmul(a2, 11) ^ gmul(a3, 13);
                a[2] = gmul(a0, 13) ^ gmul(a1, 9) ^ gmul(a2, 14) ^ gmul(a3, 11);
                a[3] = gmul(a0, 11) ^ gmul(a1, 13) ^ gmul(a2, 9) ^ gmul(a3, 14);
            }
        }
    }
}

/* ── Keys ────────────────────────────────────────────────────────────── */

/* GID stand-in (Key 0x837), generated at build time from KEY837
 * (see the Makefile); the key is not part of the source tree. */
static const u8 key_0x837[16] = {
#include "../key837.inc"
};
static const u8 key_uid[16] = {             /* qemu-ios ipod_touch_aes.h */
    0x01,0x23,0x45,0x67,0x89,0xAB,0xCD,0xEF,
    0x01,0x23,0x45,0x67,0x89,0xAB,0xCD,0xEF,
};

static void be_words_to_bytes(u8 *out, const u32 *w, u32 nwords) {
    for (u32 i = 0; i < nwords; i++) {
        out[4 * i + 0] = (u8)(w[i] >> 24);
        out[4 * i + 1] = (u8)(w[i] >> 16);
        out[4 * i + 2] = (u8)(w[i] >> 8);
        out[4 * i + 3] = (u8)w[i];
    }
}

/* VA for a DMA address (kernel RAM through the linear map). */
static u8 *dma_va(u32 addr, u32 len) {
    if (addr >= 0x80000000u)            /* qemu-ios: bus alias of DRAM */
        addr -= 0x80000000u;
    if (addr - 0x21000000u < 0x07000000u &&
        addr + len - 0x21000000u <= 0x07000000u)
        return (u8 *)io_va(addr);
    return 0;
}

/* ── Operation ───────────────────────────────────────────────────────── */

static void aes_log_once(u32 sel, u32 keybits, bool enc, u32 len) {
    static u32 seen;
    u32 bit = 1u << ((sel & 3) * 2 + (enc ? 1 : 0));
    if (seen & bit)
        return;
    seen |= bit;
    static const char *const name[4] = { "custom", "GID", "UID", "?" };
    console_print("[AES] ");
    console_print(name[sel & 3]);
    console_print(enc ? " enc " : " dec ");
    console_print_hex(keybits);
    console_print(" bits len=");
    console_print_hex(len);
    console_print(sel == 1 ? " (0x837 stands in)\n" : "\n");
}

static void aes_go(void) {
    u32 mode = regs[AES_MODE / 4];
    bool enc = mode & 1;
    u32 sz = (mode >> 4) & 3;
    u32 keybits = sz == 2 ? 256 : sz == 1 ? 192 : 128;
    u32 sel = regs[AES_KEYSEL / 4];
    u32 len = regs[AES_LEN / 4] & ~15u;

    u8 key[32];
    if (sel == 1) {
        keybits = 128;
        for (u32 i = 0; i < 16; i++) key[i] = key_0x837[i];
    } else if (sel == 2) {
        keybits = 128;
        for (u32 i = 0; i < 16; i++) key[i] = key_uid[i];
    } else {
        u32 nwords = keybits / 32;
        be_words_to_bytes(key, &regs[AES_KEY / 4 + (8 - nwords)], nwords);
    }
    aes_log_once(sel, keybits, enc, len);

    u8 iv[16];
    be_words_to_bytes(iv, &regs[AES_IV / 4], 4);

    u8 *in = dma_va(regs[AES_INADDR / 4], len);
    u8 *out = dma_va(regs[AES_OUTADDR / 4], len);
    if (!in || !out) {
        static bool shown;
        if (!shown) {
            shown = true;
            console_print("[AES] bad DMA in=");
            console_print_hex(regs[AES_INADDR / 4]);
            console_print(" out=");
            console_print_hex(regs[AES_OUTADDR / 4]);
            console_print("\n");
        }
        return;
    }

    static aes_ctx_t ctx;
    aes_expand(&ctx, key, keybits);
    for (u32 off = 0; off < len; off += 16) {
        u8 blk[16];
        for (u32 i = 0; i < 16; i++)
            blk[i] = in[off + i];
        if (enc) {
            for (u32 i = 0; i < 16; i++)
                blk[i] ^= iv[i];
            aes_encrypt_block(&ctx, blk);
            for (u32 i = 0; i < 16; i++)
                iv[i] = blk[i];
        } else {
            u8 c[16];
            for (u32 i = 0; i < 16; i++)
                c[i] = blk[i];
            aes_decrypt_block(&ctx, blk);
            for (u32 i = 0; i < 16; i++) {
                blk[i] ^= iv[i];
                iv[i] = c[i];
            }
        }
        for (u32 i = 0; i < 16; i++)
            out[off + i] = blk[i];
    }

    /* A real engine writes RAM; the driver may invalidate (not clean) the
     * output range afterwards, so push our cached writes out. */
    for (u32 a = (u32)out & ~31u; a < (u32)out + len; a += 32)
        __asm__ volatile("mcr p15, 0, %0, c7, c10, 1" :: "r"(a));
    __asm__ volatile("mcr p15, 0, %0, c7, c10, 4" :: "r"(0) : "memory");
}

void aes_init(void) {
    for (u32 i = 0; i < 256; i++)
        inv_sbox[sbox[i]] = (u8)i;
}

u32 aes_read(u32 off) {
    return off < sizeof(regs) ? regs[off / 4] : 0;
}

void aes_write(u32 off, u32 value) {
    if (off >= sizeof(regs))
        return;
    switch (off) {
    case AES_GO:
        if (value & 1) {
            aes_go();
            regs[AES_STATUS / 4] = 0xF;
        }
        return;
    case AES_CONTROL:
    case AES_RESET:
        regs[AES_STATUS / 4] = 0;
        break;
    }
    regs[off / 4] = value;
}
