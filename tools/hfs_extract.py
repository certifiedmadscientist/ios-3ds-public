#!/usr/bin/env python3
"""Minimal read-only HFS+/HFSX reader (stdlib only).

  hfs_extract.py IMG ls PATH            list a directory
  hfs_extract.py IMG get PATH OUT       extract a file's data fork
  hfs_extract.py IMG extent PATH        print a file's data-fork extents as
                                        absolute byte ranges in IMG

IMG may be a bare volume or an Apple Partition Map disk image (the first
Apple_HFS/Apple_HFSX partition is used).
"""
import mmap
import struct
import sys


class HFS:
    def __init__(self, path):
        self.f = open(path, 'rb')
        self.d = mmap.mmap(self.f.fileno(), 0, access=mmap.ACCESS_READ)
        self.base = self._find_volume()
        vh = self.d[self.base + 1024:self.base + 1536]
        assert vh[:2] in (b'H+', b'HX'), vh[:2]
        self.bs = struct.unpack_from('>I', vh, 0x28)[0]
        self.vh = vh
        self.cat = self._fork_data(self._fork_extents(vh, 0x110),
                                   struct.unpack_from('>Q', vh, 0x110)[0])
        self.nsz = struct.unpack_from('>H', self.cat, 14 + 18)[0]
        self._index()

    def _find_volume(self):
        d = self.d
        if d[1024:1026] in (b'H+', b'HX'):
            return 0
        if d[512:514] == b'PM':
            n = struct.unpack_from('>I', d, 512 + 4)[0]
            for i in range(n):
                e = 512 * (1 + i)
                if d[e:e + 2] != b'PM':
                    continue
                start, = struct.unpack_from('>I', d, e + 8)
                ptype = d[e + 48:e + 80].split(b'\0')[0]
                if ptype in (b'Apple_HFS', b'Apple_HFSX'):
                    return start * 512
        raise SystemExit('no HFS+ volume found')

    @staticmethod
    def _fork_extents(buf, off):
        return [struct.unpack_from('>II', buf, off + 16 + 8 * i) for i in range(8)]

    def _fork_data(self, ext, size):
        out = b''.join(self.d[self.base + s * self.bs:self.base + (s + c) * self.bs]
                       for s, c in ext if c)
        return out[:size]

    def _index(self):
        self.names, self.parent, self.kind, self.fork = {}, {}, {}, {}
        cat, nsz = self.cat, self.nsz
        n = struct.unpack_from('>I', cat, 14 + 10)[0]
        while n:
            node = cat[n * nsz:(n + 1) * nsz]
            flink, = struct.unpack_from('>I', node, 0)
            nrec, = struct.unpack_from('>H', node, 10)
            for i in range(nrec):
                o, = struct.unpack_from('>H', node, nsz - 2 * (i + 1))
                klen, = struct.unpack_from('>H', node, o)
                pid, nlen = struct.unpack_from('>IH', node, o + 2)
                name = node[o + 8:o + 8 + 2 * nlen].decode('utf-16-be', 'replace')
                r = o + 2 + klen
                r += r & 1
                rtype, = struct.unpack_from('>h', node, r)
                if rtype not in (1, 2):
                    continue
                cnid, = struct.unpack_from('>I', node, r + 8)
                self.names[cnid], self.parent[cnid] = name, pid
                self.kind[cnid] = 'd' if rtype == 1 else 'f'
                if rtype == 2:
                    size, = struct.unpack_from('>Q', node, r + 88)
                    self.fork[cnid] = (size, self._fork_extents(node, r + 88))
            n = flink

    def lookup(self, path):
        cur = 2
        for part in [p for p in path.split('/') if p]:
            kids = [c for c, p in self.parent.items()
                    if p == cur and self.names[c] == part]
            if not kids:
                raise SystemExit(f'not found: {path}')
            cur = kids[0]
        return cur

    def ls(self, path):
        cur = self.lookup(path)
        return sorted((self.names[c], self.kind[c], self.fork.get(c, (0,))[0])
                      for c, p in self.parent.items() if p == cur)

    def read(self, path):
        size, ext = self.fork[self.lookup(path)]
        if sum(c for _, c in ext) * self.bs < size:
            raise SystemExit(f'{path}: needs extents-overflow file (unsupported)')
        return self._fork_data(ext, size)

    def extents(self, path):
        size, ext = self.fork[self.lookup(path)]
        return size, [(self.base + s * self.bs, c * self.bs) for s, c in ext if c]


def main():
    img, cmd, path = sys.argv[1:4]
    h = HFS(img)
    if cmd == 'ls':
        for name, kind, size in h.ls(path):
            print(f'{kind} {size:>10} {name}')
    elif cmd == 'get':
        open(sys.argv[4], 'wb').write(h.read(path))
    elif cmd == 'extent':
        size, ranges = h.extents(path)
        print(size, ' '.join(f'{o:#x}+{n:#x}' for o, n in ranges))


if __name__ == '__main__':
    main()
