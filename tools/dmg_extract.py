#!/usr/bin/env python3
"""Decrypt an iPhone OS 1.x root filesystem DMG and write its HFS+ partition
as a bare volume image.

  dmg_extract.py --key HEX ROOTFS.dmg OUT.img

Two layers, both implemented here so no external tools are needed:

  1. FileVault "encrcdsa" (version 2) encryption.  HEX is the 72-hex-digit
     VFDecrypt key for the DMG (16-byte AES-128 key followed by a 20-byte
     HMAC-SHA1 key).  Each block is AES-128-CBC with
     IV = HMAC-SHA1(hmac_key, big-endian block number)[:16].
  2. A UDIF image (koly trailer, blkx/mish block tables, zlib/ADC/bzip2/raw/
     zero chunks).  Only the Apple_HFS / Apple_HFSX partition is written.

Needs the Python 'cryptography' package (or PyCryptodome).
"""
import argparse
import bz2
import hashlib
import hmac
import os
import plistlib
import struct
import sys
import tempfile
import zlib

SECTOR = 512


def aes_cbc_decryptor(key):
    try:
        from cryptography.hazmat.primitives.ciphers import Cipher, algorithms, modes

        def dec(data, iv):
            d = Cipher(algorithms.AES(key), modes.CBC(iv)).decryptor()
            return d.update(data) + d.finalize()
        return dec
    except ImportError:
        pass
    try:
        from Crypto.Cipher import AES

        def dec(data, iv):
            return AES.new(key, AES.MODE_CBC, iv).decrypt(data)
        return dec
    except ImportError:
        sys.exit('need the Python "cryptography" (or "pycryptodome") package')


def vfdecrypt(src, dst, key_hex):
    """encrcdsa v2 -> plain UDIF file."""
    key = bytes.fromhex(key_hex)
    if len(key) != 36:
        sys.exit('rootfs key must be 72 hex digits (AES-128 key + HMAC-SHA1 key)')
    aes_key, hmac_key = key[:16], key[16:]
    with open(src, 'rb') as f:
        hdr = f.read(0x48)
        if hdr[:8] != b'encrcdsa':
            return False
        version, = struct.unpack_from('>I', hdr, 8)
        if version != 2:
            sys.exit(f'unsupported encrcdsa version {version}')
        bs, = struct.unpack_from('>I', hdr, 0x34)
        size, off = struct.unpack_from('>QQ', hdr, 0x38)
        dec = aes_cbc_decryptor(aes_key)
        f.seek(off)
        left, n = size, 0
        with open(dst, 'wb') as o:
            while left > 0:
                blk = f.read(bs)
                if len(blk) != bs:
                    sys.exit('encrypted DMG is truncated')
                iv = hmac.new(hmac_key, struct.pack('>I', n), hashlib.sha1).digest()[:16]
                plain = dec(blk, iv)
                o.write(plain[:min(bs, left)])
                left -= bs
                n += 1
    return True


def adc_decompress(src, out_len):
    out = bytearray()
    i = 0
    while i < len(src) and len(out) < out_len:
        b = src[i]
        if b & 0x80:                                   # literal run
            n = (b & 0x7F) + 1
            out += src[i + 1:i + 1 + n]
            i += 1 + n
            continue
        if b & 0x40:                                   # 3-byte copy
            n = (b & 0x3F) + 4
            dist = (src[i + 1] << 8) | src[i + 2]
            i += 3
        else:                                          # 2-byte copy
            n = ((b >> 2) & 0x0F) + 3
            dist = ((b & 3) << 8) | src[i + 1]
            i += 2
        for _ in range(n):
            out.append(out[-dist - 1])
    return bytes(out)


def udif_to_raw(src, dst):
    with open(src, 'rb') as f:
        f.seek(-512, os.SEEK_END)
        koly = f.read(512)
        if koly[:4] != b'koly':
            sys.exit('no UDIF koly trailer: wrong rootfs key?')
        data_fork_off, = struct.unpack_from('>Q', koly, 0x18)
        xml_off, xml_len = struct.unpack_from('>QQ', koly, 0xD8)
        f.seek(xml_off)
        pl = plistlib.loads(f.read(xml_len))
        parts = pl['resource-fork']['blkx']
        part = [p for p in parts if 'Apple_HFS' in p.get('Name', '')]
        if len(part) != 1:
            sys.exit('expected exactly one Apple_HFS partition, found: ' +
                     ', '.join(p.get('Name', '?') for p in parts))
        mish = part[0]['Data']
        assert mish[:4] == b'mish', 'bad blkx block table'
        sect_count, = struct.unpack_from('>Q', mish, 0x10)
        base, = struct.unpack_from('>Q', mish, 0x18)
        nchunks, = struct.unpack_from('>I', mish, 0xC8)
        with open(dst, 'wb') as o:
            o.truncate(sect_count * SECTOR)
            for i in range(nchunks):
                typ, _, sn, sc, co, cl = struct.unpack_from('>IIQQQQ', mish, 0xCC + 40 * i)
                if typ in (0x00000000, 0x00000002, 0x7FFFFFFE):
                    continue                          # zero / ignore / comment
                if typ == 0xFFFFFFFF:
                    break
                f.seek(data_fork_off + base + co)
                raw = f.read(cl)
                want = sc * SECTOR
                if typ == 0x00000001:
                    out = raw
                elif typ == 0x80000005:
                    out = zlib.decompress(raw)
                elif typ == 0x80000004:
                    out = adc_decompress(raw, want)
                elif typ == 0x80000006:
                    out = bz2.decompress(raw)
                else:
                    sys.exit(f'unsupported UDIF chunk type {typ:#x}')
                if len(out) != want:
                    sys.exit(f'chunk {i}: {len(out)} bytes, expected {want}')
                o.seek(sn * SECTOR)
                o.write(out)
    return sect_count * SECTOR


def main():
    ap = argparse.ArgumentParser(description=__doc__.split('\n')[0])
    ap.add_argument('--key', required=True, help='VFDecrypt key (72 hex digits)')
    ap.add_argument('dmg')
    ap.add_argument('out')
    a = ap.parse_args()
    fd, tmp = tempfile.mkstemp(suffix='.udif', dir=os.path.dirname(os.path.abspath(a.out)))
    os.close(fd)
    try:
        udif = tmp if vfdecrypt(a.dmg, tmp, a.key) else a.dmg
        size = udif_to_raw(udif, a.out)
    finally:
        os.unlink(tmp)
    with open(a.out, 'rb') as f:
        f.seek(1024)
        sig = f.read(2)
    if sig not in (b'H+', b'HX'):
        sys.exit('output has no HFS+ volume header: wrong rootfs key?')
    print(f'{a.out}: {size} bytes ({sig.decode()} volume)')


if __name__ == '__main__':
    main()
