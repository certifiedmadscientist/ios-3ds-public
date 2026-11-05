#!/usr/bin/env python3
"""
extract_devicetree.py - Extract the flattened Apple DeviceTree from an
iPod touch 1G (S5L8900X) DeviceTree.n45ap.img2 firmware file.

Pipeline: strip the 8900 header, AES-128-CBC decrypt with key 0x837,
then take the IMG2 payload (it starts at 0x400; its size is at 0x10).
The result is the raw tree iBoot would hand to the kernel, with
iBoot's placeholder properties still zero-filled.

Usage:
  python3 -I extract_devicetree.py --key837 HEX <DeviceTree.n45ap.img2> <out.devicetree>
"""

import importlib.util
import os
import struct
import sys

_here = os.path.dirname(os.path.abspath(__file__))
_spec = importlib.util.spec_from_file_location(
    "extract_kernelcache", os.path.join(_here, "extract_kernelcache.py"))
kc = importlib.util.module_from_spec(_spec)
_spec.loader.exec_module(kc)


def main():
    key837 = kc.pop_key837(sys.argv)
    if len(sys.argv) != 3:
        sys.exit(__doc__)
    raw = open(sys.argv[1], "rb").read()
    fmt, _, payload = kc.parse_8900_header(raw)
    if fmt == kc.FORMAT_ENCRYPTED_0x837:
        payload = kc.decrypt_aes128_cbc(payload, key837, kc.ZERO_IV)
    if payload[:4] != kc.IMG2_MAGIC:
        sys.exit("decrypted payload is not IMG2")
    size = struct.unpack_from("<I", payload, 0x10)[0]
    tree = payload[kc.IMG2_HEADER_SIZE:kc.IMG2_HEADER_SIZE + size]
    if len(tree) != size:
        sys.exit("truncated IMG2 payload")
    open(sys.argv[2], "wb").write(tree)
    print(f"wrote {size} bytes to {sys.argv[2]}")


if __name__ == "__main__":
    main()
