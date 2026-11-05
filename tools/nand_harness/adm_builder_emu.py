#!/usr/bin/env python3
"""Run the kernel's ADM read-command builder on a FIL request and dump the
command block it produces.

  adm_builder_emu.py KERNEL.BIN BANK PAGE [META]

0xC04AC248 (AppleS5L8900XADMFMC) turns the request built by the FIL's
common read (0xC046AB9C) into the ADM command block our nand.c decodes.
The request mirrors a FIL +20 read (flags 5, one page, start index 0,
META bytes of metadata per page: 0 for +20, 12 for +4).  The submit
(0xC04ABF9C) and result check (0xC04AB93C) are stubbed to succeed, and the
IOMemoryDescriptor is a stub whose physical segment is one 4 KB buffer.
"""
import struct
import sys

from unicorn import Uc, UcError, UC_ARCH_ARM, UC_MODE_ARM, UC_HOOK_CODE
from unicorn.arm_const import (UC_ARM_REG_R0, UC_ARM_REG_R1, UC_ARM_REG_R2, UC_ARM_REG_SP,
                               UC_ARM_REG_LR, UC_ARM_REG_PC, UC_ARM_REG_CPSR)

KERNEL, BANK, PAGE = sys.argv[1], int(sys.argv[2], 0), int(sys.argv[3], 0)
META = int(sys.argv[4], 0) if len(sys.argv) > 4 else 0

BUILDER, SUBMIT, RESULT = 0xC04AC248, 0xC04ABF9C, 0xC04AB93C
PRINTF, UDIV = 0xC0020306, None
HEAP, STACK, STUBS = 0xD0000000, 0xD1000000, 0xF0000000

mu = Uc(UC_ARCH_ARM, UC_MODE_ARM)
mu.mem_map(0xC0000000, 0x00700000)
kd = open(KERNEL, 'rb').read()
for va, fo, sz in ((0xC0008000, 0, 0x19D000), (0xC01A5000, 0x19D000, 0x1B000),
                   (0xC0000000, 0x1B8000, 0x5000), (0xC0264000, 0x1BD000, 0x37000),
                   (0xC029B000, 0x1F4000, 0x366000)):
    mu.mem_write(va, kd[fo:fo + sz])
mu.mem_map(HEAP, 0x00100000)
mu.mem_map(STACK, 0x00100000)
mu.mem_map(STUBS, 0x1000)
mu.mem_write(STUBS, struct.pack('<I', 0xE12FFF1E) * 1024)


def w32(a, v): mu.mem_write(a, struct.pack('<I', v & 0xFFFFFFFF))
def w16(a, v): mu.mem_write(a, struct.pack('<H', v & 0xFFFF))
def r32(a): return struct.unpack('<I', mu.mem_read(a, 4))[0]
def be32(a): return struct.unpack('>I', mu.mem_read(a, 4))[0]
def be16(a): return struct.unpack('>H', mu.mem_read(a, 2))[0]


drv, cmd, req, pages, banks, md, vt = (HEAP + 0x0000, HEAP + 0x10000, HEAP + 0x2000,
                                       HEAP + 0x2100, HEAP + 0x2200, HEAP + 0x2300, HEAP + 0x2400)
w32(drv + 0xF4, cmd)
w32(drv + 0xB4, 8)                      # chip enables
for i in range(8):
    w32(drv + 0x90 + 4 * i, i)          # CE map
w32(drv + 0xC0, 0x800)                  # bytes per page
w32(req + 0x04, 5)                      # flags as 0xC046AB9C sets them for +20
w32(req + 0x08, 1)                      # pages
w32(req + 0x0C, 0)                      # start index
w32(req + 0x10, pages)
w32(req + 0x14, md)
w32(req + 0x1C, banks)
w32(req + 0x24, META)
w32(pages, PAGE)
w16(banks, BANK)
w32(md, vt)                             # IOMemoryDescriptor: vtable only
for i in range(64):
    w32(vt + 4 * i, STUBS + 4 * i)
mu.mem_write(cmd, b'\xA5' * 0x2000)


def ret(uc, v):
    uc.reg_write(UC_ARM_REG_R0, v & 0xFFFFFFFF)
    lr = uc.reg_read(UC_ARM_REG_LR)
    c = uc.reg_read(UC_ARM_REG_CPSR)
    uc.reg_write(UC_ARM_REG_CPSR, (c | 0x20) if lr & 1 else (c & ~0x20))
    uc.reg_write(UC_ARM_REG_PC, lr & ~1)


def hook(uc, addr, size, _):
    if addr == SUBMIT:
        return ret(uc, 1)
    if addr == RESULT:
        return ret(uc, 0)
    if addr == PRINTF:
        return ret(uc, 0)
    if STUBS <= addr < STUBS + 0x400:
        slot = (addr - STUBS) // 4
        if slot == 0x98 // 4:           # getPhysicalSegment(offset, &length)
            off, lenp = uc.reg_read(UC_ARM_REG_R1), uc.reg_read(UC_ARM_REG_R2)
            if lenp:
                w32(lenp, 0x1000 - off if off < 0x1000 else 0)
            return ret(uc, 0x237CE000 + off if off < 0x1000 else 0)
        return ret(uc, 0)


mu.hook_add(UC_HOOK_CODE, hook)
mu.reg_write(UC_ARM_REG_R0, drv)
mu.reg_write(UC_ARM_REG_R1, req)
mu.reg_write(UC_ARM_REG_SP, STACK + 0xF0000)
mu.reg_write(UC_ARM_REG_LR, STUBS + 0xFF0)
mu.reg_write(UC_ARM_REG_CPSR, 0x13)
try:
    mu.emu_start(BUILDER, STUBS + 0xFF0, count=2_000_000)
except UcError as e:
    print('!! %s at pc %#x' % (e, mu.reg_read(UC_ARM_REG_PC)))

print('request: bank %d page %#x meta %d' % (BANK, PAGE, META))
print('  +0x24 opcode %#06x  +0x26 %#06x  +0x28 npages %d  +0x2C %#x  +0x30 %#x' %
      (struct.unpack('<H', mu.mem_read(cmd + 0x24, 2))[0], struct.unpack('<H', mu.mem_read(cmd + 0x26, 2))[0],
       be16(cmd + 0x28), be16(cmd + 0x2C), be16(cmd + 0x30)))
print('  +0x44 bank %d  +0x244 page %#x  sg0 %#x/%#x  sg1 %#x/%#x' %
      (mu.mem_read(cmd + 0x44, 1)[0], be32(cmd + 0x244), be32(cmd + 0xA44), be32(cmd + 0xA48),
       be32(cmd + 0xA4C), be32(cmd + 0xA50)))
