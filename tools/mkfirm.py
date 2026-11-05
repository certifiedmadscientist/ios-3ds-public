#!/usr/bin/env python3
"""
mkfirm.py — Package ARM11/ARM9 binaries into a FIRM file for Luma3DS chainloading.

FIRM format (0x200-byte header + section data):
  0x000: "FIRM" magic
  0x004: Boot priority (u32 LE)
  0x008: ARM11 entry point (u32 LE)
  0x00C: ARM9 entry point (u32 LE)
  0x010: Reserved (0x30 bytes)
  0x040: Section table — 4 entries x 0x30 bytes each:
         0x00: File offset (u32 LE)
         0x04: Load address (u32 LE)
         0x08: Size (u32 LE)
         0x0C: Copy method (u32 LE): 0=ARM9, 1=ARM11
         0x10: SHA-256 hash (32 bytes)
  0x100: Signature area (0x100 bytes, zeroed for custom payloads)
  0x200: Section data begins (each section aligned to 0x200)
"""

import argparse
import hashlib
import struct
import sys

# ── ELF parser (minimal, no pyelftools needed) ──────────────────────────

ELF_MAGIC = b'\x7fELF'
ET_EXEC = 2
EM_ARM = 40
PT_LOAD = 1

def parse_elf(path):
    """Parse an ARM ELF file. Returns (entry_point, [(paddr, data), ...])."""
    with open(path, 'rb') as f:
        blob = f.read()

    if blob[:4] != ELF_MAGIC:
        raise ValueError(f"{path}: not an ELF file")

    ei_class = blob[4]
    if ei_class != 1:
        raise ValueError(f"{path}: not a 32-bit ELF (ei_class={ei_class})")

    ei_data = blob[5]
    if ei_data != 1:
        raise ValueError(f"{path}: not little-endian (ei_data={ei_data})")

    # ELF32 header fields
    (e_type, e_machine, e_version, e_entry, e_phoff, e_shoff,
     e_flags, e_ehsize, e_phentsize, e_phnum, e_shentsize, e_shnum,
     e_shstrndx) = struct.unpack_from('<HHIIIIIHHHHHH', blob, 16)

    if e_machine != EM_ARM:
        raise ValueError(f"{path}: not ARM (e_machine={e_machine})")

    segments = []
    for i in range(e_phnum):
        off = e_phoff + i * e_phentsize
        (p_type, p_offset, p_vaddr, p_paddr, p_filesz, p_memsz,
         p_flags, p_align) = struct.unpack_from('<IIIIIIII', blob, off)

        if p_type != PT_LOAD:
            continue
        if p_filesz == 0:
            continue

        data = blob[p_offset:p_offset + p_filesz]
        # If memsz > filesz, pad with zeros (BSS in the segment)
        if p_memsz > p_filesz:
            data += b'\x00' * (p_memsz - p_filesz)

        segments.append((p_paddr, data))

    if not segments:
        raise ValueError(f"{path}: no loadable segments found")

    return e_entry, segments


def load_binary(path, addr):
    """Load a raw binary file at the given address."""
    with open(path, 'rb') as f:
        data = f.read()
    return addr, [(addr, data)]


# ── FIRM builder ─────────────────────────────────────────────────────────

FIRM_HEADER_SIZE = 0x200
SECTION_ALIGN = 0x200

# sighax signature — passes boot9's RSA check on B9S consoles
SIGHAX_SIG = bytes.fromhex(
    'b6724531c448657a2a2ee306457e350a10d544b42859b0e5b0bed27534cccc2a'
    '4d47edea60a7dd99939950a6357b1e35dfc7fac773b7e12e7c1481234af141b3'
    '1cf08e9f62293aa6baae246c15095f8b78402a684d852c680549fa5b3f14d9e8'
    '38a2fb9c09a15abb40dca25e40a3ddc1f58e79cec901974363a946e99b4346e8'
    'a372b6cd55a707e1eab9bec0200b5ba0b661236a8708d704517f43c6c38ee956'
    '0111e1405e5e8ed356c49c4ff6823d1219afaeeb3df3c36b62bba88fc15ba864'
    '8f9333fd9fc092b8146c3d908f73155d48be89d72612e18e4aa8eb9b7fd2a5f7'
    '328c4ecbfb0083833cbd5c983a25ceb8b941cc68eb017ce87f5d793aca09acf7'
)

def align_up(val, alignment):
    return (val + alignment - 1) & ~(alignment - 1)


def build_firm(arm11_entry, arm11_segments, arm9_entry=0, arm9_segments=None,
               stub11_segs=None):
    """Build a FIRM file from loaded segments. Returns bytes."""

    # Merge all segments into section entries: (load_addr, data, processor_type)
    sections = []

    # ARM9 sections first (type=0, copied by ARM9)
    if arm9_segments:
        for paddr, data in arm9_segments:
            sections.append((paddr, data, 0))  # 0 = ARM9

    # ARM11 main code (type=0 — ARM9 copies it to FCRAM)
    for paddr, data in arm11_segments:
        sections.append((paddr, data, 0))  # 0 = ARM9-copied

    # ARM11 stub in AXIWRAM (type=1 — small, non-cacheable)
    if stub11_segs:
        for paddr, data in stub11_segs:
            sections.append((paddr, data, 1))  # 1 = ARM11

    if len(sections) > 4:
        raise ValueError(f"FIRM supports at most 4 sections, got {len(sections)}")

    # Build header
    header = bytearray(FIRM_HEADER_SIZE)

    # Magic
    header[0x000:0x004] = b'FIRM'
    # Boot priority
    struct.pack_into('<I', header, 0x004, 0)
    # ARM11 entry point
    struct.pack_into('<I', header, 0x008, arm11_entry)
    # ARM9 entry point
    struct.pack_into('<I', header, 0x00C, arm9_entry)
    # Reserved 0x010..0x03F already zero
    # Sighax signature at 0x100..0x1FF
    header[0x100:0x200] = SIGHAX_SIG

    # Compute section offsets and fill section table
    data_offset = FIRM_HEADER_SIZE
    section_blobs = []

    for i, (load_addr, data, proc_type) in enumerate(sections):
        # Pad data to alignment
        padded_size = align_up(len(data), SECTION_ALIGN)
        padded_data = data + b'\x00' * (padded_size - len(data))

        sha256 = hashlib.sha256(padded_data).digest()

        tbl_off = 0x040 + i * 0x30
        struct.pack_into('<I', header, tbl_off + 0x00, data_offset)
        struct.pack_into('<I', header, tbl_off + 0x04, load_addr)
        struct.pack_into('<I', header, tbl_off + 0x08, padded_size)
        struct.pack_into('<I', header, tbl_off + 0x0C, proc_type)
        header[tbl_off + 0x10:tbl_off + 0x30] = sha256

        section_blobs.append(padded_data)
        data_offset += padded_size

    return bytes(header) + b''.join(section_blobs)


# ── CLI ──────────────────────────────────────────────────────────────────

def parse_addr(s):
    """Parse an address string (hex or decimal)."""
    if s.startswith('0x') or s.startswith('0X'):
        return int(s, 16)
    return int(s)


def main():
    parser = argparse.ArgumentParser(
        description='Package ARM11/ARM9 binaries into a FIRM file for Luma3DS.')

    # ARM11 — required
    arm11 = parser.add_mutually_exclusive_group(required=True)
    arm11.add_argument('--arm11', metavar='FILE.ELF',
                       help='ARM11 ELF file')
    arm11.add_argument('--arm11-bin', metavar='FILE.BIN',
                       help='ARM11 raw binary file')
    parser.add_argument('--arm11-addr', metavar='ADDR',
                        help='Load address for --arm11-bin (e.g. 0x20000000)')

    # ARM11 stub — optional, loaded to AXIWRAM for cache-safe ARM11 entry
    parser.add_argument('--stub11-bin', metavar='FILE.BIN',
                        help='ARM11 stub binary (loaded to AXIWRAM)')
    parser.add_argument('--stub11-addr', metavar='ADDR',
                        help='Load address for stub11 (e.g. 0x1FF80000)')

    # ARM9 — optional
    arm9 = parser.add_mutually_exclusive_group()
    arm9.add_argument('--arm9', metavar='FILE.ELF',
                      help='ARM9 ELF file')
    arm9.add_argument('--arm9-bin', metavar='FILE.BIN',
                      help='ARM9 raw binary file')
    parser.add_argument('--arm9-addr', metavar='ADDR',
                        help='Load address for --arm9-bin (e.g. 0x08000000)')

    parser.add_argument('--output', '-o', required=True, metavar='PAYLOAD.FIRM',
                        help='Output FIRM file path')

    args = parser.parse_args()

    # Load ARM11 stub (entry point for ARM11 — goes to AXIWRAM)
    stub11_entry = 0
    stub11_segs = None
    if args.stub11_bin:
        if not args.stub11_addr:
            parser.error("--stub11-addr is required with --stub11-bin")
        addr = parse_addr(args.stub11_addr)
        stub11_entry, stub11_segs = load_binary(args.stub11_bin, addr)
        print(f"ARM11 stub: entry=0x{stub11_entry:08X}, size={len(stub11_segs[0][1])} bytes")

    # Load ARM11 main code
    if args.arm11:
        arm11_entry, arm11_segs = parse_elf(args.arm11)
        print(f"ARM11 ELF: entry=0x{arm11_entry:08X}, {len(arm11_segs)} segment(s)")
    else:
        if not args.arm11_addr:
            parser.error("--arm11-addr is required with --arm11-bin")
        addr = parse_addr(args.arm11_addr)
        arm11_entry, arm11_segs = load_binary(args.arm11_bin, addr)
        print(f"ARM11 bin: entry=0x{arm11_entry:08X}, size={len(arm11_segs[0][1])} bytes")

    # Load ARM9
    arm9_entry = 0
    arm9_segs = None
    if args.arm9:
        arm9_entry, arm9_segs = parse_elf(args.arm9)
        print(f"ARM9 ELF: entry=0x{arm9_entry:08X}, {len(arm9_segs)} segment(s)")
    elif args.arm9_bin:
        if not args.arm9_addr:
            parser.error("--arm9-addr is required with --arm9-bin")
        addr = parse_addr(args.arm9_addr)
        arm9_entry, arm9_segs = load_binary(args.arm9_bin, addr)
        print(f"ARM9 bin: entry=0x{arm9_entry:08X}, size={len(arm9_segs[0][1])} bytes")

    # If stub11 is provided, ARM11 enters at stub which jumps to main code
    if stub11_segs:
        real_arm11_entry = stub11_entry
    else:
        real_arm11_entry = arm11_entry

    # Build FIRM
    firm = build_firm(real_arm11_entry, arm11_segs, arm9_entry, arm9_segs,
                      stub11_segs=stub11_segs)

    with open(args.output, 'wb') as f:
        f.write(firm)

    print(f"Wrote {len(firm)} bytes to {args.output}")
    print(f"  Header:  0x000-0x1FF")
    print(f"  Payload: 0x200-0x{len(firm)-1:X}")


if __name__ == '__main__':
    main()
