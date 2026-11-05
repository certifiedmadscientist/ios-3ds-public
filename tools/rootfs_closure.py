#!/usr/bin/env python3
"""Dependency closure + size budget for a real iPhone OS userland.

  rootfs_closure.py ROOTFS.img [--list OUT.txt]

ROOTFS.img is the decrypted rootfs (raw HFS+ or APM image). Starting from
/sbin/launchd, the LaunchDaemons it would start and SpringBoard, follows
LC_LOAD_DYLIB / LC_LOAD_WEAK_DYLIB / LC_REEXPORT_DYLIB (and the dylinker)
through HFS+ symlinks, then adds the resource trees SpringBoard needs.
Prints per-item and total byte counts; --list writes every rootfs path the
closure needs (one per line, directories expanded) for an image builder.
"""
import os
import plistlib
import struct
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from hfs_extract import HFS  # noqa: E402

LC_LOAD_DYLIB, LC_ID_DYLIB, LC_LOAD_DYLINKER = 0xC, 0xD, 0xE
LC_LOAD_WEAK_DYLIB, LC_REEXPORT_DYLIB = 0x80000018, 0x8000001F


class Rootfs(HFS):
    def _index(self):
        super()._index()
        self.kids = {}
        for c, p in self.parent.items():
            self.kids.setdefault(p, {})[self.names[c]] = c
        # HFS+ symlinks: file records with Finder type 'slnk'.
        self.slnk = set()
        cat, nsz = self.cat, self.nsz
        n = struct.unpack_from('>I', cat, 14 + 10)[0]
        while n:
            node = cat[n * nsz:(n + 1) * nsz]
            flink, = struct.unpack_from('>I', node, 0)
            nrec, = struct.unpack_from('>H', node, 10)
            for i in range(nrec):
                o, = struct.unpack_from('>H', node, nsz - 2 * (i + 1))
                klen, = struct.unpack_from('>H', node, o)
                r = o + 2 + klen
                r += r & 1
                if struct.unpack_from('>h', node, r)[0] == 2:
                    cnid, = struct.unpack_from('>I', node, r + 8)
                    if node[r + 48:r + 52] == b'slnk':
                        self.slnk.add(cnid)
            n = flink

    def resolve(self, path, depth=0):
        """Follow symlinks in every component; return (cnid, real_path)."""
        cur, parts, done = 2, [p for p in path.split('/') if p], []
        while parts:
            part = parts.pop(0)
            if part == '.':
                continue
            if part == '..':
                done = done[:-1]
                cur = 2
                for d in done:
                    cur = self.kids[cur][d]
                continue
            c = self.kids.get(cur, {}).get(part)
            if c is None:
                return None, '/' + '/'.join(done + [part] + parts)
            if c in self.slnk and depth < 32:
                tgt = self._fork_data(self.fork[c][1], self.fork[c][0]).decode()
                base = [] if tgt.startswith('/') else done
                return self.resolve('/' + '/'.join(base + [tgt] + parts), depth + 1)
            done.append(part)
            cur = c
        return cur, '/' + '/'.join(done)

    def head(self, cnid, n=65536):
        size, ext = self.fork[cnid]
        return self._fork_data(ext, min(size, n))

    def tree(self, cnid, prefix):
        """Yield (path, size) for every file under a directory (no symlink follow)."""
        for name, c in self.kids.get(cnid, {}).items():
            p = prefix + '/' + name
            if self.kind[c] == 'd':
                yield from self.tree(c, p)
            else:
                yield p, self.fork.get(c, (0,))[0]


def macho_deps(blob):
    magic, = struct.unpack_from('<I', blob, 0)
    if magic != 0xFEEDFACE:
        return []
    ncmds, = struct.unpack_from('<I', blob, 16)
    off, deps = 28, []
    for _ in range(ncmds):
        cmd, size = struct.unpack_from('<II', blob, off)
        if cmd in (LC_LOAD_DYLIB, LC_LOAD_WEAK_DYLIB, LC_REEXPORT_DYLIB, LC_LOAD_DYLINKER):
            name_off, = struct.unpack_from('<I', blob, off + 8)
            deps.append(blob[off + name_off:off + size].split(b'\0')[0].decode())
        off += size
    return deps


def daemon_programs(fs):
    progs = {}
    ld, _ = fs.resolve('/System/Library/LaunchDaemons')
    for name, c in sorted(fs.kids[ld].items()):
        pl = plistlib.loads(fs.head(c, 1 << 20))
        args = pl.get('ProgramArguments') or [pl.get('Program')]
        progs[pl.get('Label', name)] = (args[0], bool(pl.get('Disabled')),
                                         sorted(pl.get('MachServices', {})))
    return progs


def main():
    fs = Rootfs(sys.argv[1])
    out_list = sys.argv[sys.argv.index('--list') + 1] if '--list' in sys.argv else None

    progs = daemon_programs(fs)
    print('LaunchDaemons:')
    for label, (prog, disabled, svcs) in progs.items():
        print(f'  {label:36} {"(disabled) " if disabled else ""}{prog}  {",".join(svcs)}')

    roots = ['/sbin/launchd'] + [p for p, d, _ in progs.values() if not d]
    seen, sizes, missing, order = set(), {}, set(), []
    stack = list(roots)
    while stack:
        path = stack.pop()
        if path.startswith('@'):
            continue
        cnid, real = fs.resolve(path)
        if cnid is None:
            missing.add(path)
            continue
        if real in seen:
            continue
        seen.add(real)
        order.append(real)
        sizes[real] = fs.fork[cnid][0]
        stack.extend(macho_deps(fs.head(cnid)))

    bins = sum(sizes.values())
    print(f'\nMach-O closure: {len(sizes)} files, {bins:,} bytes')
    for p, s in sorted(sizes.items(), key=lambda x: -x[1])[:15]:
        print(f'  {s:>10,}  {p}')
    if missing:
        print('  missing:', ', '.join(sorted(missing)))

    # Resource trees: every bundle in the closure plus SpringBoard's needs.
    bundles = set()
    for p in sizes:
        for marker in ('.framework/', '.app/', '.bundle/'):
            if marker in p:
                bundles.add(p[:p.index(marker) + len(marker) - 1])
    extra = ['/System/Library/CoreServices/SpringBoard.app', '/System/Library/Fonts',
             '/System/Library/CoreServices/SystemVersion.plist', '/Library/Preferences',
             '/private/var', '/private/etc', '/System/Library/LaunchDaemons',
             '/usr/share/zoneinfo', '/System/Library/Frameworks/UIKit.framework',
             '/System/Library/PrivateFrameworks/GraphicsServices.framework']
    files = dict(sizes)
    contrib = {}
    for root in sorted(bundles) + extra:
        cnid, real = fs.resolve(root)
        if cnid is None:
            continue
        if fs.kind[cnid] == 'f':
            files[real] = fs.fork[cnid][0]
            continue
        t = dict(fs.tree(cnid, real))
        contrib[real] = sum(t.values())
        files.update(t)
    total = sum(files.values())
    print(f'\nWith bundles/resources: {len(files)} files, {total:,} bytes')
    for p, s in sorted(contrib.items(), key=lambda x: -x[1])[:15]:
        print(f'  {s:>12,}  {p}')
    whole = sum(s for _, s in fs.tree(2, ''))
    print(f'\nWhole rootfs: {whole:,} bytes')

    if out_list:
        with open(out_list, 'w') as f:
            f.write('\n'.join(sorted(files)) + '\n')


if __name__ == '__main__':
    main()
