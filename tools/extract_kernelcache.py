#!/usr/bin/env python3
"""
extract_kernelcache.py - Extract kernelcache from iPod touch 1G (S5L8900X) firmware

Handles the full extraction pipeline:
  1. Strip 8900 header (2048 bytes)
  2. Decrypt AES-128-CBC with Key 0x837 (if format byte == 0x03)
  3. Parse IMG2 container to find DATA tag
  4. Decompress Apple LZSS (complzss) payload
  5. Verify Mach-O output

Usage:
  python3 -I extract_kernelcache.py --key837 HEX <input_kernelcache> <output_macho>

The S5L8900 "Key 0x837" is not included; pass it with --key837 (32 hex
digits; see the README for where it is documented).
"""

import struct
import sys
import os

# ---------------------------------------------------------------------------
# IMG2 files in the 8900 wrapper are AES-128-CBC encrypted (zero IV) with the
# S5L8900 "Key 0x837".  The key is supplied by the user (--key837).
# ---------------------------------------------------------------------------
ZERO_IV = b"\x00" * 16


def parse_key837(text):
    """Return the 16-byte Key 0x837 from a hex string, or exit."""
    try:
        key = bytes.fromhex(text.strip())
    except ValueError:
        key = b""
    if len(key) != 16:
        sys.exit("Key 0x837 must be 32 hex digits")
    return key


def pop_key837(argv):
    """Remove '--key837 HEX' from argv and return the key bytes."""
    if "--key837" not in argv:
        sys.exit("missing --key837 HEX (the S5L8900 Key 0x837)")
    i = argv.index("--key837")
    if i + 1 >= len(argv):
        sys.exit("missing value for --key837")
    key = parse_key837(argv[i + 1])
    del argv[i:i + 2]
    return key

# ---------------------------------------------------------------------------
# 8900 header constants
# ---------------------------------------------------------------------------
HEADER_8900_MAGIC = b"8900"
HEADER_8900_SIZE = 0x800  # 2048 bytes
FORMAT_ENCRYPTED_0x837 = 0x03
FORMAT_PLAINTEXT = 0x04

# ---------------------------------------------------------------------------
# IMG2 constants
# ---------------------------------------------------------------------------
IMG2_MAGIC = b"2gmI"  # "Img2" little-endian
IMG2_HEADER_SIZE = 0x400  # 1024 bytes

# IMG3 constants (for completeness)
IMG3_MAGIC = b"3gmI"  # "Img3" little-endian

# Tag identifiers
TAG_DATA = b"ATAD"  # "DATA" little-endian
TAG_TYPE = b"EPYT"  # "TYPE" little-endian
TAG_VERS = b"SREV"  # "VERS" little-endian

# ---------------------------------------------------------------------------
# Mach-O constants
# ---------------------------------------------------------------------------
MACHO_MAGIC = 0xFEEDFACE
MACHO_MAGIC_64 = 0xFEEDFACF
CPU_TYPE_ARM = 12


def decrypt_aes128_cbc(data, key, iv):
    """Decrypt AES-128-CBC using Python's hashlib/hmac or cryptography lib."""
    try:
        from cryptography.hazmat.primitives.ciphers import Cipher, algorithms, modes
        cipher = Cipher(algorithms.AES(key), modes.CBC(iv))
        decryptor = cipher.decryptor()
        return decryptor.update(data) + decryptor.finalize()
    except ImportError:
        pass

    # Fallback: try PyCryptodome
    try:
        from Crypto.Cipher import AES
        cipher = AES.new(key, AES.MODE_CBC, iv)
        return cipher.decrypt(data)
    except ImportError:
        pass

    # Fallback: use openssl via subprocess
    import subprocess
    import tempfile
    key_hex = key.hex()
    iv_hex = iv.hex()
    with tempfile.NamedTemporaryFile(delete=False, suffix=".enc") as tmp_in:
        tmp_in.write(data)
        tmp_in_path = tmp_in.name
    tmp_out_path = tmp_in_path + ".dec"
    try:
        subprocess.run([
            "openssl", "enc", "-d", "-aes-128-cbc",
            "-K", key_hex, "-iv", iv_hex,
            "-nopad",
            "-in", tmp_in_path,
            "-out", tmp_out_path
        ], check=True, capture_output=True)
        with open(tmp_out_path, "rb") as f:
            return f.read()
    finally:
        for p in [tmp_in_path, tmp_out_path]:
            try:
                os.unlink(p)
            except OSError:
                pass


def decompress_lzss(data, uncompressed_size):
    """
    Apple's LZSS decompression -- matches the XNU/iBoot implementation exactly.

    Uses a 4096-byte ring buffer initialized with spaces (0x20) at positions
    0 through N-F-1. The write cursor starts at N-F (=4078). Control flags
    are read LSB-first. Back-references use absolute ring buffer positions
    (12 bits), not distances.

    Reference: XNU osfmk/x86_64/loose_ends.c decompress_lzss()
    """
    N = 4096          # ring buffer size (must be power of 2)
    F = 18            # lookahead buffer size
    THRESHOLD = 2     # minimum match length before encoding as reference

    # Ring buffer, initialized with spaces at 0..N-F-1
    text_buf = bytearray(b' ' * (N + F - 1))
    r = N - F  # write cursor in ring buffer

    result = bytearray()
    src = 0
    srclen = len(data)
    flags = 0

    while len(result) < uncompressed_size:
        # When the sentinel bit (bit 8) has been shifted out, read new flags
        flags >>= 1
        if (flags & 0x100) == 0:
            if src >= srclen:
                break
            flags = data[src] | 0xFF00
            src += 1

        if flags & 1:
            # Literal byte
            if src >= srclen:
                break
            c = data[src]
            src += 1
            result.append(c)
            text_buf[r] = c
            r = (r + 1) & (N - 1)
        else:
            # Back-reference: 12-bit ring position + 4-bit length
            if src >= srclen:
                break
            i = data[src]
            src += 1
            if src >= srclen:
                break
            j = data[src]
            src += 1
            # i = low 8 bits of position; high 4 bits come from j's upper nibble
            i |= (j & 0xF0) << 4
            # length = low nibble of j + THRESHOLD + 1 (loop is 0..j inclusive)
            j = (j & 0x0F) + THRESHOLD
            for k in range(j + 1):
                c = text_buf[(i + k) & (N - 1)]
                result.append(c)
                text_buf[r] = c
                r = (r + 1) & (N - 1)

    return bytes(result[:uncompressed_size])


def parse_8900_header(data):
    """Parse the 8900 file header. Returns (format_byte, data_size, payload_data)."""
    if len(data) < HEADER_8900_SIZE:
        raise ValueError(f"File too small for 8900 header: {len(data)} bytes")

    magic = data[0:4]
    if magic != HEADER_8900_MAGIC:
        raise ValueError(f"Not an 8900 file. Magic: {magic!r}")

    version = data[4:7].decode("ascii", errors="replace")
    format_byte = data[7]
    data_size = struct.unpack_from("<I", data, 0x0C)[0]
    footer_sig_off = struct.unpack_from("<I", data, 0x10)[0]
    footer_cert_off = struct.unpack_from("<I", data, 0x14)[0]
    footer_cert_len = struct.unpack_from("<I", data, 0x18)[0]

    print(f"  8900 Header:")
    print(f"    Version: {version}")
    print(f"    Format: 0x{format_byte:02x} ({'encrypted (0x837 key)' if format_byte == FORMAT_ENCRYPTED_0x837 else 'plaintext' if format_byte == FORMAT_PLAINTEXT else 'unknown'})")
    print(f"    Data size: {data_size} bytes ({data_size / 1024 / 1024:.2f} MB)")
    print(f"    Footer sig offset: 0x{footer_sig_off:x}")
    print(f"    Footer cert offset: 0x{footer_cert_off:x}")
    print(f"    Footer cert length: {footer_cert_len}")

    payload = data[HEADER_8900_SIZE:HEADER_8900_SIZE + data_size]
    return format_byte, data_size, payload


def parse_img2(data):
    """Parse an IMG2 container. Returns the DATA payload."""
    if len(data) < 4:
        raise ValueError("Data too small for IMG2")

    magic = data[0:4]
    if magic != IMG2_MAGIC:
        # Maybe it's not IMG2, check if it's directly complzss or Mach-O
        if data[0:8] == b"complzss":
            print("  No IMG2 wrapper found -- data is directly complzss compressed")
            return data
        macho_magic = struct.unpack_from("<I", data, 0)[0]
        if macho_magic == MACHO_MAGIC:
            print("  No IMG2 wrapper found -- data is directly a Mach-O binary")
            return data
        raise ValueError(f"Not an IMG2 file. Magic bytes: {data[0:4].hex()} ('{data[0:4]}')")

    # IMG2 header
    # 0x00: 4 bytes magic "2gmI"
    # 0x04: 4 bytes imageType (e.g., "krnl", "logo", "ibot", etc.)
    # ...
    # The IMG2 header is typically 0x400 bytes, but the actual structure
    # varies. The DATA is usually right after the header.
    image_type = data[4:8]
    print(f"  IMG2 container:")
    print(f"    Type: {image_type[::-1].decode('ascii', errors='replace')}")

    # Try to parse IMG2 -- the header is complex, but data typically
    # starts at offset 0x400 for standard IMG2 files
    # However, some IMG2 files have the data at different offsets.
    # Let's search for complzss or Mach-O magic after the IMG2 header.

    # First try the standard offset
    for test_offset in [0x400, 0x3EC]:
        if test_offset < len(data):
            test_magic = data[test_offset:test_offset + 8]
            if test_magic == b"complzss":
                print(f"    Found complzss at offset 0x{test_offset:x}")
                return data[test_offset:]
            test_macho = struct.unpack_from("<I", data, test_offset)[0] if test_offset + 4 <= len(data) else 0
            if test_macho == MACHO_MAGIC:
                print(f"    Found Mach-O at offset 0x{test_offset:x}")
                return data[test_offset:]

    # Search for magic
    for magic_bytes, name in [(b"complzss", "complzss"), (b"\xce\xfa\xed\xfe", "Mach-O")]:
        pos = data.find(magic_bytes)
        if pos >= 0:
            print(f"    Found {name} at offset 0x{pos:x}")
            return data[pos:]

    # If nothing found, try returning data after IMG2 header
    print(f"    WARNING: No complzss or Mach-O found in IMG2, returning data at offset 0x400")
    return data[0x400:]


def parse_img3(data):
    """Parse an IMG3 container. Returns the DATA payload."""
    if data[0:4] != IMG3_MAGIC:
        raise ValueError(f"Not an IMG3 file. Magic: {data[0:4].hex()}")

    full_size = struct.unpack_from("<I", data, 4)[0]
    data_size = struct.unpack_from("<I", data, 8)[0]
    signed_size = struct.unpack_from("<I", data, 12)[0]
    type_tag = struct.unpack_from("<I", data, 16)[0]

    print(f"  IMG3 container:")
    print(f"    Full size: {full_size}")
    print(f"    Data size: {data_size}")
    print(f"    Signed size: {signed_size}")
    print(f"    Type: 0x{type_tag:08x}")

    # Parse tags starting at offset 20
    offset = 20
    while offset < len(data) - 12:
        tag_magic = data[offset:offset + 4]
        tag_total_size = struct.unpack_from("<I", data, offset + 4)[0]
        tag_data_size = struct.unpack_from("<I", data, offset + 8)[0]

        tag_name = tag_magic[::-1].decode("ascii", errors="replace")
        print(f"    Tag '{tag_name}' at 0x{offset:x}: total={tag_total_size}, data={tag_data_size}")

        if tag_magic == TAG_DATA:
            return data[offset + 12:offset + 12 + tag_data_size]

        if tag_total_size == 0:
            break
        offset += tag_total_size
        # Align to 4 bytes
        offset = (offset + 3) & ~3

    raise ValueError("DATA tag not found in IMG3")


def handle_complzss(data):
    """Parse and decompress a complzss payload."""
    if data[0:8] != b"complzss":
        raise ValueError(f"Not complzss data. Header: {data[0:8].hex()}")

    # complzss header:
    # 0x00: 8 bytes magic "complzss"
    # 0x08: 4 bytes checksum (Adler32 of uncompressed data)
    # 0x0C: 4 bytes uncompressed size (big-endian)
    # 0x10: 4 bytes compressed size (big-endian)
    # 0x14: 4 bytes unknown/padding
    # 0x180: compressed data starts (the header is padded to 0x180)

    # Actually the header layout varies. Let me check common layouts.
    # Some references say:
    # 0x00: "complzss" (8 bytes)
    # 0x08: adler32 (4 bytes, big-endian)
    # 0x0C: uncompressed_size (4 bytes, big-endian)
    # 0x10: compressed_size (4 bytes, big-endian)
    # 0x14: unknown (4 bytes)
    # Then padding up to 0x180
    # Compressed data at 0x180

    adler32 = struct.unpack_from(">I", data, 0x08)[0]
    uncompressed_size = struct.unpack_from(">I", data, 0x0C)[0]
    compressed_size = struct.unpack_from(">I", data, 0x10)[0]

    print(f"  complzss header:")
    print(f"    Adler32: 0x{adler32:08x}")
    print(f"    Uncompressed size: {uncompressed_size} bytes ({uncompressed_size / 1024 / 1024:.2f} MB)")
    print(f"    Compressed size: {compressed_size} bytes ({compressed_size / 1024 / 1024:.2f} MB)")

    # The compressed data offset varies:
    # - In some implementations it's at 0x180 (with a padded header)
    # - In others it's at 0x18 (right after the header fields)
    # Let's try to find the right offset by checking for valid LZSS data

    # Standard offset is 0x180 for Apple's kernelcache complzss
    compressed_offset = 0x180
    if compressed_offset >= len(data):
        # Try smaller offset
        compressed_offset = 0x18
        if compressed_offset >= len(data):
            compressed_offset = 0x14

    compressed_data = data[compressed_offset:compressed_offset + compressed_size]
    if len(compressed_data) < compressed_size:
        # Maybe the offset is wrong, try the other one
        alt_offset = 0x18 if compressed_offset == 0x180 else 0x180
        if alt_offset + compressed_size <= len(data):
            compressed_data = data[alt_offset:alt_offset + compressed_size]
            compressed_offset = alt_offset
        else:
            print(f"    WARNING: compressed data shorter than expected ({len(compressed_data)} < {compressed_size})")

    print(f"    Compressed data offset: 0x{compressed_offset:x}")
    print(f"    Actual compressed data available: {len(compressed_data)} bytes")

    print(f"  Decompressing LZSS ({compressed_size} -> {uncompressed_size} bytes)...")
    decompressed = decompress_lzss(compressed_data, uncompressed_size)
    print(f"  Decompressed: {len(decompressed)} bytes")

    return decompressed


def verify_macho(data):
    """Verify the data is a valid Mach-O binary and print header info."""
    if len(data) < 28:
        print("  WARNING: Data too small to be a valid Mach-O")
        return False

    magic = struct.unpack_from("<I", data, 0)[0]
    if magic == MACHO_MAGIC:
        cputype = struct.unpack_from("<I", data, 4)[0]
        cpusubtype = struct.unpack_from("<I", data, 8)[0]
        filetype = struct.unpack_from("<I", data, 12)[0]
        ncmds = struct.unpack_from("<I", data, 16)[0]
        sizeofcmds = struct.unpack_from("<I", data, 20)[0]
        flags = struct.unpack_from("<I", data, 24)[0]

        print(f"  Mach-O verification:")
        print(f"    Magic: 0xFEEDFACE (32-bit)")
        print(f"    CPU type: {cputype} ({'ARM' if cputype == CPU_TYPE_ARM else 'unknown'})")
        print(f"    CPU subtype: {cpusubtype}")
        print(f"    File type: {filetype}")
        print(f"    Number of load commands: {ncmds}")
        print(f"    Size of load commands: {sizeofcmds}")
        print(f"    Flags: 0x{flags:08x}")

        if cputype == CPU_TYPE_ARM:
            print("    VALID: ARM Mach-O kernel binary")
            return True
        else:
            print(f"    WARNING: Expected ARM (12), got cputype={cputype}")
            return True
    elif magic == MACHO_MAGIC_64:
        print("  Mach-O 64-bit detected (unexpected for iPod touch 1G)")
        return True
    else:
        print(f"  WARNING: Not a Mach-O binary. Magic: 0x{magic:08x}")
        # Print first 32 bytes for debugging
        print(f"  First 32 bytes: {data[:32].hex()}")
        return False


def main():
    key837 = pop_key837(sys.argv)
    if len(sys.argv) < 3:
        print(f"Usage: {sys.argv[0]} --key837 HEX <input_kernelcache> <output_macho>")
        print(f"  Extract and decompress a kernelcache from iPod touch 1G firmware")
        sys.exit(1)

    input_path = sys.argv[1]
    output_path = sys.argv[2]

    print(f"Reading {input_path}...")
    with open(input_path, "rb") as f:
        raw_data = f.read()
    print(f"  File size: {len(raw_data)} bytes ({len(raw_data) / 1024 / 1024:.2f} MB)")

    # Step 1: Detect and handle file format
    magic = raw_data[0:4]

    if magic == HEADER_8900_MAGIC:
        print("\nStep 1: Parsing 8900 header...")
        format_byte, data_size, payload = parse_8900_header(raw_data)

        if format_byte == FORMAT_ENCRYPTED_0x837:
            print("\nStep 2: Decrypting with Key 0x837 (AES-128-CBC)...")
            # Pad payload to AES block size if needed
            pad_len = (16 - (len(payload) % 16)) % 16
            if pad_len:
                payload = payload + b"\x00" * pad_len
            payload = decrypt_aes128_cbc(payload, key837, ZERO_IV)
            print(f"  Decrypted: {len(payload)} bytes")
            print(f"  First 16 bytes: {payload[:16].hex()}")
        elif format_byte == FORMAT_PLAINTEXT:
            print("\nStep 2: Data is plaintext (no decryption needed)")
        else:
            print(f"\nStep 2: Unknown format byte 0x{format_byte:02x}, trying as plaintext...")

    elif magic == IMG3_MAGIC:
        print("\nIMG3 format detected (skipping 8900)...")
        payload = raw_data
    elif magic == IMG2_MAGIC:
        print("\nIMG2 format detected (no 8900 wrapper)...")
        payload = raw_data
    elif raw_data[0:8] == b"complzss":
        print("\nRaw complzss detected (no wrapper)...")
        payload = raw_data
    else:
        print(f"\nUnknown format, magic: {magic.hex()} - trying as raw data...")
        payload = raw_data

    # Step 3: Handle container format (IMG2 or IMG3)
    inner_magic = payload[0:4]

    if inner_magic == IMG2_MAGIC:
        print("\nStep 3: Parsing IMG2 container...")
        kernel_data = parse_img2(payload)
    elif inner_magic == IMG3_MAGIC:
        print("\nStep 3: Parsing IMG3 container...")
        kernel_data = parse_img3(payload)
    elif payload[0:8] == b"complzss":
        print("\nStep 3: No container, data is directly complzss")
        kernel_data = payload
    elif struct.unpack_from("<I", payload, 0)[0] == MACHO_MAGIC:
        print("\nStep 3: No container, data is directly Mach-O")
        kernel_data = payload
    else:
        print(f"\nStep 3: Unknown inner format: {payload[:16].hex()}")
        # Try to search for known magic in the decrypted data
        for search_magic, name in [(b"complzss", "complzss"), (b"\xce\xfa\xed\xfe", "Mach-O"), (IMG2_MAGIC, "IMG2")]:
            pos = payload.find(search_magic)
            if pos >= 0:
                print(f"  Found {name} at offset 0x{pos:x}")
                kernel_data = payload[pos:]
                break
        else:
            print("  ERROR: Could not find any recognizable format in decrypted data")
            print(f"  First 64 bytes: {payload[:64].hex()}")
            # Save the decrypted payload for debugging
            debug_path = output_path + ".decrypted_raw"
            with open(debug_path, "wb") as f:
                f.write(payload)
            print(f"  Saved raw decrypted data to {debug_path} for debugging")
            sys.exit(1)

    # Step 4: Handle complzss compression
    if kernel_data[0:8] == b"complzss":
        print("\nStep 4: Decompressing complzss...")
        kernel_data = handle_complzss(kernel_data)
    else:
        print("\nStep 4: Data is not complzss-compressed")

    # Step 5: Verify Mach-O
    print("\nStep 5: Verifying Mach-O...")
    is_valid = verify_macho(kernel_data)

    # Step 6: Save
    print(f"\nStep 6: Saving to {output_path}...")
    with open(output_path, "wb") as f:
        f.write(kernel_data)
    print(f"  Wrote {len(kernel_data)} bytes ({len(kernel_data) / 1024 / 1024:.2f} MB)")

    if is_valid:
        print("\nDone! Kernelcache extracted successfully.")
    else:
        print("\nDone! WARNING: Output may not be a valid Mach-O. Check the file manually.")

    return 0 if is_valid else 1


if __name__ == "__main__":
    sys.exit(main())
