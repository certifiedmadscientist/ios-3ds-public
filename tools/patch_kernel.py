#!/usr/bin/env python3
"""Apply a byte patch list (patches/kernel.patches) to the decrypted kernelcache.

  patch_kernel.py PATCHES IN.macho OUT.bin [--force]

Each hunk's original bytes are verified before any byte is written; a
mismatch aborts without output.  The list may name the md5 of the expected
input and output ("input-md5", "output-md5"); a mismatch there is reported
and is fatal unless --force is given (the per-hunk checks still apply).
"""
import hashlib
import sys


def parse(path):
    hunks, meta, cur = [], {}, None
    for n, raw in enumerate(open(path), 1):
        line = raw.split('#', 1)[0].strip()
        if not line:
            continue
        tag, _, rest = line.partition(' ')
        rest = rest.strip()
        if tag in ('input-md5', 'output-md5'):
            meta[tag] = rest.lower()
        elif tag == '@':
            cur = [int(rest, 0), bytearray(), bytearray(), n]
            hunks.append(cur)
        elif tag in ('-', '+') and cur is not None:
            if tag == '-' and rest.startswith('zeros '):
                data = bytes(int(rest.split()[1], 0))
            else:
                data = bytes.fromhex(rest)
            (cur[1] if tag == '-' else cur[2]).extend(data)
        else:
            sys.exit(f'{path}:{n}: cannot parse: {raw.rstrip()}')
    for off, old, new, n in hunks:
        if len(old) != len(new) or not old:
            sys.exit(f'{path}:{n}: hunk @ {off:#x}: {len(old)} original vs {len(new)} new bytes')
    return hunks, meta


def main():
    args = [a for a in sys.argv[1:] if a != '--force']
    force = len(args) != len(sys.argv) - 1
    if len(args) != 3:
        sys.exit(__doc__)
    plist, src, dst = args
    hunks, meta = parse(plist)
    buf = bytearray(open(src, 'rb').read())

    def check(tag, data):
        want = meta.get(tag)
        have = hashlib.md5(data).hexdigest()
        if want and want != have:
            msg = f'{tag}: have {have}, patch list expects {want}'
            if not force:
                sys.exit(msg + ' (use --force to continue)')
            print('WARNING ' + msg)
        return have

    check('input-md5', buf)
    bad = []
    for off, old, new, n in hunks:
        have = bytes(buf[off:off + len(old)])
        if have != old:
            bad.append(f'  @ {off:#x} (line {n}): have {have[:16].hex()}..., expected {bytes(old[:16]).hex()}...')
    if bad:
        sys.exit('original bytes do not match; nothing written:\n' + '\n'.join(bad))
    for off, old, new, n in hunks:
        buf[off:off + len(new)] = new
    md5 = check('output-md5', buf)
    open(dst, 'wb').write(buf)
    print(f'{dst}: {len(hunks)} hunks, {sum(len(h[2]) for h in hunks)} bytes, md5 {md5}')


if __name__ == '__main__':
    main()
