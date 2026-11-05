#!/usr/bin/env python3
"""Run the kernel's own VFL context loader against our NAND emulation.

  vfl_emu.py KERNEL.BIN libnandemu.so DISK.IMG [--trace]

Maps the kernelcache segments at their real VAs in Unicorn, sets the NAND
geometry the FIL would report (8 banks x 4096 blocks x 128 pages of 2 KB),
runs the geometry setup (0xC0478ED0) and the VFL context loader that
VFL_Open calls (0xC0477B00: per bank, the BBT loader 0xC0476110 and the
context search/selection), and prints everything the kernel prints.
FIL page reads are served by nand_get_page() from libnandemu.so, built with
  cc -O2 -shared -fPIC -ffreestanding -fno-builtin -Iboot boot/periph/nand.c \
     boot/periph/pl080.c tools/nand_harness/hal_host.c -o libnandemu.so
Run with Unicorn (pip install unicorn) under python -I.  FIL_RET_ERASED=<code>
makes blank-page reads return that FIL status.
"""
import ctypes
import os
import struct
import sys

from unicorn import Uc, UcError, UC_ARCH_ARM, UC_MODE_ARM, UC_HOOK_CODE, UC_HOOK_MEM_UNMAPPED
from unicorn.arm_const import (UC_ARM_REG_R0, UC_ARM_REG_R1, UC_ARM_REG_R2, UC_ARM_REG_R3,
                               UC_ARM_REG_SP, UC_ARM_REG_LR, UC_ARM_REG_PC, UC_ARM_REG_CPSR)

KERNEL, LIB, DISK = sys.argv[1:4]
TRACE = '--trace' in sys.argv

SEGS = [  # vmaddr, fileoff, filesize
    (0xC0008000, 0x000000, 0x19D000),   # __TEXT
    (0xC01A5000, 0x19D000, 0x01B000),   # __DATA
    (0xC0000000, 0x1B8000, 0x005000),   # __HIB
    (0xC0264000, 0x1BD000, 0x037000),   # __KLD
    (0xC029B000, 0x1F4000, 0x366000),   # __PRELINK
]
HEAP, STACK, STUBS = 0xD0000000, 0xD1000000, 0xF0000000

PRINTF, KALLOC, MEMCMP = 0xC0020306, 0xC012E0A4, 0xC046D4A8
BUF_GET, BUF_RELEASE = 0xC0475508, 0xC0475610
G, FIL_TABLE_PTR, BITMAP_PTR, CXT_ARRAY_PTR, CB_STRUCT = (
    0xC047D344, 0xC047D2E8, 0xC047D2F4, 0xC047D2E4, 0xC047D33C)
GEOMETRY_FN, LOAD_CXT_FN = 0xC0478ED0, 0xC0477B00

# ── our NAND emulation ────────────────────────────────────────────────
lib = ctypes.CDLL(os.path.abspath(LIB))
fd = os.open(DISK, os.O_RDONLY)
ctypes.c_int.in_dll(lib, 'hal_disk_fd').value = fd
ctypes.c_uint64.in_dll(lib, 'hal_disk_size').value = os.lseek(fd, 0, os.SEEK_END)
lib.nand_init()


def nand_page(bank, pn):
    """Page as the kernel would receive it: ADM read + PL080 FIFO copy."""
    d, s = ctypes.create_string_buffer(2048), ctypes.create_string_buffer(64)
    lib.emu_fil_read(bank, pn, d, s)
    ref_d, ref_s = ctypes.create_string_buffer(2048), ctypes.create_string_buffer(64)
    lib.nand_get_page(bank, pn, ref_d, ref_s)
    if d.raw != ref_d.raw or s.raw != ref_s.raw:
        print('!! transport mismatch bank %d page %#x (%s)' %
              (bank, pn, 'data' if d.raw != ref_d.raw else 'spare'))
    return d.raw, s.raw


# ── machine ───────────────────────────────────────────────────────────
mu = Uc(UC_ARCH_ARM, UC_MODE_ARM)
mu.mem_map(0xC0000000, 0x00700000)
kd = open(KERNEL, 'rb').read()
for va, fo, sz in SEGS:
    mu.mem_write(va, kd[fo:fo + sz])
mu.mem_map(HEAP, 0x01000000)
mu.mem_map(STACK, 0x00100000)
mu.mem_map(STUBS, 0x1000)
mu.mem_write(STUBS, struct.pack('<I', 0xE12FFF1E) * 1024)       # bx lr
heap_next = [HEAP]


def alloc(n):
    p = heap_next[0]
    heap_next[0] += (n + 63) & ~63
    mu.mem_write(p, bytes(n))
    return p


def r32(a): return struct.unpack('<I', mu.mem_read(a, 4))[0]
def w32(a, v): mu.mem_write(a, struct.pack('<I', v & 0xFFFFFFFF))
def w16(a, v): mu.mem_write(a, struct.pack('<H', v & 0xFFFF))


def cstr(a):
    out = b''
    while len(out) < 512:
        c = bytes(mu.mem_read(a + len(out), 1))
        if c == b'\0':
            break
        out += c
    return out.decode('latin1')


def ret(uc, val=None):
    if val is not None:
        uc.reg_write(UC_ARM_REG_R0, val & 0xFFFFFFFF)
    lr = uc.reg_read(UC_ARM_REG_LR)
    cpsr = uc.reg_read(UC_ARM_REG_CPSR)
    uc.reg_write(UC_ARM_REG_CPSR, (cpsr | 0x20) if lr & 1 else (cpsr & ~0x20))
    uc.reg_write(UC_ARM_REG_PC, lr & ~1)


def printf(uc):
    fmt = cstr(uc.reg_read(UC_ARM_REG_R0))
    args = [uc.reg_read(r) for r in (UC_ARM_REG_R1, UC_ARM_REG_R2, UC_ARM_REG_R3)]
    sp = uc.reg_read(UC_ARM_REG_SP)
    args += [r32(sp + 4 * i) for i in range(8)]
    out, i, ai = '', 0, 0
    while i < len(fmt):
        c = fmt[i]
        if c != '%':
            out += c
            i += 1
            continue
        j = i + 1
        while j < len(fmt) and fmt[j] in '0123456789-l#.':
            j += 1
        conv, spec = fmt[j], fmt[i:j + 1].replace('l', '')
        v = args[ai] if ai < len(args) else 0
        ai += 1
        if conv == 's':
            out += cstr(v)
        elif conv in 'dixXuc':
            out += (spec.replace('u', 'd') % (v - (1 << 32) if conv in 'di' and v >> 31 else v))
        elif conv == 'p':
            out += '0x%08x' % v
        else:
            out += spec
        i = j + 1
    sys.stdout.write('  kernel| ' + out if not out.startswith('\n') else out)
    if not out.endswith('\n'):
        sys.stdout.write('\n')
    ret(uc, 0)


reads = []
FIL_RET_ERASED = int(os.environ.get('FIL_RET_ERASED', '0'), 0)


def fil(uc, idx):
    r0, r1, r2, r3 = (uc.reg_read(r) for r in (UC_ARM_REG_R0, UC_ARM_REG_R1, UC_ARM_REG_R2, UC_ARM_REG_R3))
    if idx in (1, 5):                   # +4: read(bank, page, data, spare); +20: read(bank, page, data)
        bank, pn = r0 & 0xFFFF, r1
        data, spare = nand_page(bank, pn)
        if r2:
            mu.mem_write(r2, data)
        if idx == 1 and r3:
            mu.mem_write(r3, spare)
        erased = data == b'\xff' * 2048 or (data == bytes(2048) and spare[9] == 0)
        rv = FIL_RET_ERASED if erased and FIL_RET_ERASED else 0
        reads.append((idx, bank, pn, spare[8], spare[9], rv))
        if TRACE:
            print('    FIL%+d read bank %d page %d (blk %d pg %d) spare[8..10]=%s -> %#x' %
                  (idx * 4, bank, pn, pn // 128, pn % 128, spare[8:11].hex(), rv))
        return ret(uc, rv)
    print('    FIL table entry %d called (r0=%#x r1=%#x): returning 0' % (idx, r0, r1))
    ret(uc, 0)


last_pcs = []


def hook(uc, addr, size, _):
    last_pcs.append(addr)
    if len(last_pcs) > 12:
        del last_pcs[0]
    if addr == PRINTF:
        return printf(uc)
    if addr == KALLOC:
        return ret(uc, alloc(uc.reg_read(UC_ARM_REG_R0) or 4))
    if addr == BUF_GET:
        b = alloc(8 + 2048 + 64)
        w32(b, b + 8)
        w32(b + 4, b + 8 + 2048)
        return ret(uc, b)
    if addr == BUF_RELEASE:
        return ret(uc, 0)
    if addr == MEMCMP:                  # driver-object memcmp wrapper
        a, b, n = (uc.reg_read(r) for r in (UC_ARM_REG_R0, UC_ARM_REG_R1, UC_ARM_REG_R2))
        return ret(uc, 0 if bytes(uc.mem_read(a, n)) == bytes(uc.mem_read(b, n)) else 1)
    if STUBS <= addr < STUBS + 0x1000:
        idx = (addr - STUBS) // 4
        if idx < 32:
            return fil(uc, idx)
        if idx == 40:                   # [CB_STRUCT+4]: per-bank callback
            return ret(uc, 1)
        return ret(uc, 0)


def unmapped(uc, access, addr, size, value, _):
    print('!! unmapped access %#x at pc %#x' % (addr, uc.reg_read(UC_ARM_REG_PC)))
    return False


mu.hook_add(UC_HOOK_CODE, hook)
mu.hook_add(UC_HOOK_MEM_UNMAPPED, unmapped)

# ── state the FIL / BUF / VFL init would have set up ─────────────────
table = alloc(32 * 4)
for i in range(32):
    w32(table + 4 * i, STUBS + 4 * i)
w32(FIL_TABLE_PTR, table)
w32(CB_STRUCT, STUBS + 4 * 41)      # per-bank context sync after load
w32(CB_STRUCT + 4, STUBS + 4 * 40)
BANKS, BLOCKS, PPB = 8, 4096, 128
w16(G + 0x0A, 2048 // 512)       # sectors per page
w16(G + 0x0C, PPB)
w16(G + 0x20, BLOCKS)
w16(G + 0x30, BANKS)
w32(BITMAP_PTR, alloc((BLOCKS + 7) // 8))
w32(CXT_ARRAY_PTR, alloc(BANKS * 2048))


def call(fn, *args):
    for r, v in zip((UC_ARM_REG_R0, UC_ARM_REG_R1, UC_ARM_REG_R2, UC_ARM_REG_R3), args):
        mu.reg_write(r, v)
    mu.reg_write(UC_ARM_REG_SP, STACK + 0xF0000)
    mu.reg_write(UC_ARM_REG_LR, STUBS + 0xFF0)
    mu.reg_write(UC_ARM_REG_CPSR, 0x13)
    try:
        mu.emu_start(fn, STUBS + 0xFF0, count=50_000_000)
    except UcError as e:
        print('!! emulation error %s at pc %#x; last pcs %s' %
              (e, mu.reg_read(UC_ARM_REG_PC), ' '.join('%08x' % a for a in last_pcs)))
        return None
    return mu.reg_read(UC_ARM_REG_R0)


call(GEOMETRY_FN)
L = struct.unpack('<5H', mu.mem_read(0xC047D378, 10))
print('geometry: G+0x18 blocks/bank %d, G+0x1A %d, search limit L0 %d, L = %s' %
      (struct.unpack('<H', mu.mem_read(G + 0x18, 2))[0],
       struct.unpack('<H', mu.mem_read(G + 0x1A, 2))[0], L[0], L))
rv = call(LOAD_CXT_FN)
print('VFL context loader returned %#x after %d FIL reads' % (rv if rv is not None else -1, len(reads)))
for b in range(BANKS):
    cx = bytes(mu.mem_read(r32(CXT_ARRAY_PTR) + b * 2048, 2048))
    print('  bank %d context: awInfoBlk %s, first word %#x' %
          (b, struct.unpack('<4H', cx[1954:1962]), struct.unpack('<I', cx[:4])[0]))
