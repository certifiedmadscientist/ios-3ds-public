#!/usr/bin/env python3
"""Build the NAND disk image (disk0) for real-userland bring-up.

  mk_nand_disk.py ROOTFS.img OUT_disk.img

ROOTFS.img is the decrypted iPhone OS 1.1.5 rootfs as a bare HFS+ volume.
The output is the logical disk the FTL exposes, in 2048-byte LBAs, laid out
exactly like devos50/qemu-ios-generate-nand (main branch, iPod touch 1G):

  LBA 0  MBR, one partition (sysid 0xEE) starting at LBA 3
  LBA 1  GPT header          LBA 2  GPT entry (HFS+ type)
  LBA 3+ the HFS+ volume (disk0s1)

LBA n of this file lives at FTL virtual page 206848 + n on the emulated NAND.

The volume is patched in place, the same changes qemu-ios needs:
  - fstab: root mounted rw, the /private/var partition line commented out
    (/private/var is then the directory on the root volume);
  - SpringBoard gets LK_ENABLE_MBX2D=0 (software rendering, no MBX);
  - every LaunchDaemon except the qemu-ios set is marked Disabled.
Edits stay within each file's allocated blocks; the catalog's logical size
is updated in place, so no B-tree structure changes.
"""
import os
import plistlib
import struct
import sys
import zlib

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from rootfs_closure import Rootfs  # noqa: E402

LBA = 2048
PART_LBA = 3
KEEP_DAEMONS = {'com.apple.AddressBook', 'com.apple.CommCenter', 'com.apple.configd',
                'com.apple.mobile.lockdown', 'com.apple.notifyd', 'com.apple.SpringBoard'}


class Patcher(Rootfs):
    def __init__(self, path, buf):
        super().__init__(path)
        self.buf = buf

    def _catalog_offset(self, off):
        """Map a byte offset in the catalog file to an offset in the volume."""
        for start, count in self._fork_extents(self.vh, 0x110):
            span = count * self.bs
            if off < span:
                return start * self.bs + off
            off -= span
        raise ValueError('catalog offset beyond first 8 extents')

    def _file_record(self, cnid):
        """Volume offset of the HFSPlusCatalogFile record for cnid."""
        nsz, n = self.nsz, struct.unpack_from('>I', self.cat, 14 + 10)[0]
        while n:
            node = self.cat[n * nsz:(n + 1) * nsz]
            for i in range(struct.unpack_from('>H', node, 10)[0]):
                o, = struct.unpack_from('>H', node, nsz - 2 * (i + 1))
                klen, = struct.unpack_from('>H', node, o)
                r = o + 2 + klen
                r += r & 1
                if (struct.unpack_from('>h', node, r)[0] == 2 and
                        struct.unpack_from('>I', node, r + 8)[0] == cnid):
                    return self._catalog_offset(n * nsz + r)
            n = struct.unpack_from('>I', node, 0)[0]
        raise KeyError(cnid)

    def write_file(self, path, data):
        cnid, real = self.resolve(path)
        size, ext = self.fork[cnid]
        ext = [e for e in ext if e[1]]
        cap = sum(c for _, c in ext) * self.bs
        if len(data) > cap:
            raise ValueError(f'{real}: {len(data)} bytes > {cap} allocated')
        pos = 0
        for start, count in ext:
            chunk = data[pos:pos + count * self.bs]
            o = start * self.bs
            self.buf[o:o + len(chunk)] = chunk
            # zero the slack so stale bytes past EOF never look like data
            self.buf[o + len(chunk):o + count * self.bs] = bytes(count * self.bs - len(chunk))
            pos += len(chunk)
        rec = self._file_record(cnid)
        struct.pack_into('>Q', self.buf, rec + 88, len(data))   # dataFork.logicalSize
        print(f'  patched {real}: {size} -> {len(data)} bytes')

    def _file_byte_offset(self, cnid, off):
        """Volume byte offset of file-relative offset `off` (inline extents)."""
        for start, count in self.fork[cnid][1]:
            if not count:
                continue
            span = count * self.bs
            if off < span:
                return start * self.bs + off
            off -= span
        raise ValueError('offset beyond the file\'s inline extents')

    def patch_bytes(self, path, edits):
        """Apply [(file_off, old, new)] in place; assert each old matches."""
        cnid, real = self.resolve(path)
        for off, old, new in edits:
            assert len(old) == len(new)
            vo = self._file_byte_offset(cnid, off)
            have = bytes(self.buf[vo:vo + len(old)])
            if have != old:
                raise ValueError(f'{real}+{off:#x}: have {have.hex()}, '
                                 f'expected {old.hex()}')
            self.buf[vo:vo + len(new)] = new
        print(f'  patched {real}: {len(edits)} byte edit(s)')


def disk_header(part_lbas):
    mbr = bytearray(LBA)
    struct.pack_into('<8BII', mbr, 0x1BE, 0, 0, 0, 0, 0xEE, 0, 0, 0, PART_LBA, part_lbas)
    mbr[510:512] = b'\x55\xaa'

    ent = bytearray(LBA)
    struct.pack_into('<4I', ent, 0, 0x48465300, 0x11AA0000, 0x300011AA, 0xACEC4365)
    struct.pack_into('<QQ', ent, 32, PART_LBA, PART_LBA + part_lbas - 1)  # inclusive

    hdr = bytearray(LBA)
    hdr[0:8] = b'EFI PART'
    struct.pack_into('<II', hdr, 8, 0x00010000, 0x5C)
    struct.pack_into('<Q', hdr, 72, 2)                         # hdr_lba_table
    struct.pack_into('<III', hdr, 80, 1, 0x80, zlib.crc32(bytes(ent[:0x80])))
    struct.pack_into('<I', hdr, 16, zlib.crc32(bytes(hdr[:0x5C])))
    return bytes(mbr + hdr + ent)


def main():
    src, out = sys.argv[1], sys.argv[2]
    buf = bytearray(open(src, 'rb').read())
    p = Patcher(src, buf)

    c, _ = p.resolve('/private/etc/fstab')
    fstab = p.head(c).decode().splitlines()
    fstab = [ln.replace(' ro ', ' rw ', 1) if ' / hfs ' in ln else
             ('#' + ln if '/private/var' in ln else ln) for ln in fstab]
    p.write_file('/private/etc/fstab', ('\n'.join(fstab) + '\n').encode())

    ld, _ = p.resolve('/System/Library/LaunchDaemons')
    for name in sorted(p.kids[ld]):
        path = '/System/Library/LaunchDaemons/' + name
        c, _ = p.resolve(path)
        pl = plistlib.loads(p.head(c, 1 << 20))
        label = pl.get('Label', name)
        if label == 'com.apple.SpringBoard':
            pl.setdefault('EnvironmentVariables', {})['LK_ENABLE_MBX2D'] = '0'
        elif label not in KEEP_DAEMONS:
            pl['Disabled'] = True
        else:
            continue
        p.write_file(path, plistlib.dumps(pl, fmt=plistlib.FMT_XML))

    # Get past the activation screen (lockdownd reports the device Unactivated,
    # so SpringBoard shows the iTunes "slide to activate" UI).
    #
    # A first attempt rewrote the data-ark default value "Unactivated" ->
    # "Activated". On hardware lockdownd picked up the changed string but still
    # reported Unactivated: determine_activation_state() recomputes the state
    # from the activation record / data ark (logging "determine_activation_state:
    # The activation state has not changed.") and overwrites the default. So we
    # patch that decision instead, and the default rewrite is no longer needed.
    #
    # determine_activation_state builds the computed state as a CFString in the
    # stack slot [sp,#4] through many validation branches, then stores it to two
    # places, both via `ldr r3,[sp,#4]` (file offs give vaddr = off + 0x1000):
    #     e55c:  ldr r3,[sp,#4]  ; -> lockdown_set_value(ark, "ActivationState")
    #     e58c:  ldr r3,[sp,#4]  ; -> write to the "...lockdown_cache" domain
    #                               (what _load_cached_activation_state returns)
    # lockdownd already holds a plain CFString "Activated" at vaddr 0x109d08,
    # and this function's own literal pool word 0xe64c points at it. Repoint
    # both loads from [sp,#4] to that pool word, so the stored/cached state is
    # unconditionally "Activated" regardless of the validation outcome:
    #     ldr r3,[sp,#4]      e59d3004
    #     ldr r3,[pc,#0xe8]   e59f30e8   @ e55c: pc(0xe564)+0xe8 = pool 0xe64c
    #     ldr r3,[pc,#0xb8]   e59f30b8   @ e58c: pc(0xe594)+0xb8 = pool 0xe64c
    #
    # BUT the two stores are dead code unless the computed state differs from
    # the cached one.  Just before the store block determine_activation_state
    # does:
    #     e3c8: mov  r0, sl           ; sl = old/cached state CFString
    #     e3cc: ldr  r1, [sp,#4]      ; newly computed state
    #     e3d0: bl   CFEqual
    #     e3d4: cmp  r0, #0
    #     e3d8: beq  0xe3f0           ; not equal -> run the store path
    #     e3dc: ... log "The activation state has not changed."
    #     e3ec: b    0xe5a4           ; equal -> SKIP both stores
    # On hardware the cached state and the computed state are both "Unactivated",
    # so CFEqual is true and it always took the skip at e3ec -- the e55c/e58c
    # value patches never executed.  Make the e3d8 branch unconditional so the
    # store path (and our forced "Activated" value) always runs:
    #     beq 0xe3f0          0a000004
    #     b   0xe3f0          ea000004   @ e3d8 (same imm24=4, target 0xe3f0)
    #
    # "Activated" (not "FactoryActivated") keeps lockdownd off the factory-
    # activation paths that want a DeviceCertificate. Brick state is untouched
    # (left disabled). Record validation still runs; only its result is ignored
    # for the stored state, so no absent-record lookup is newly triggered. The
    # data ark lives in the RAM FTL overlay and is recomputed every boot, so
    # nothing needs to persist. The remaining "Could not load languages list /
    # locale" warnings are non-fatal.
    #
    # (Installing another device's activation records from the qemu-ios n45ap
    # NAND was considered and rejected: those records are bound to that device's
    # keys/cert in /Library/Lockdown and would fail verify_activation_record on
    # our image; bypassing the decision in lockdownd is the robust path.)
    #
    # Even with the state forced to "Activated", hardware still showed the
    # iTunes screen and lockdownd logged "Enabling the brick state": SpringBoard
    # treats a bricked device like an unactivated one. The brick decision lives
    # in the SAME determine_activation_state, just above the activation stores:
    #     e2a0: bl   data_ark_get(brick_state)    ; r6 = cached brick object
    #     e2b8: cmp  r4, #0                        ; r4 = computed "should brick"
    #     e2bc: beq  0xe314                        ; 0a000014  r4==0 -> DISABLE
    #     e2e4: log  "Enabling the brick state"    ; r4!=0 -> set brick = TRUE
    #           ... lockdown_set_value(brick_state, kCFBooleanTrue)
    #     e314: (disable path) lockdown_set_value(brick_state, kCFBooleanFalse)
    # [e654]=kCFBooleanTrue, [e6cc]=kCFBooleanFalse (verified). Make the e2bc
    # branch unconditional so the disable path always runs; every sub-case of
    # that path stores (or leaves) kCFBooleanFalse, so BrickState is always
    # false in both the ark and the lockdown_cache domain:
    #     beq 0xe314          0a000014
    #     b   0xe314          ea000014   @ e2bc (same imm24=0x14, target 0xe314)
    #
    #   File offsets are inside /usr/libexec/lockdownd's own (single) extent;
    #   no other file is touched. vaddr = file_off + 0x1000.
    p.patch_bytes('/usr/libexec/lockdownd', [
        (0xd2bc, bytes.fromhex('1400000a'), bytes.fromhex('140000ea')),
        (0xd3d8, bytes.fromhex('0400000a'), bytes.fromhex('040000ea')),
        (0xd55c, bytes.fromhex('04309de5'), bytes.fromhex('e8309fe5')),
        (0xd58c, bytes.fromhex('04309de5'), bytes.fromhex('b8309fe5')),
    ])

    # Belt-and-braces in SpringBoard itself, so the UI never shows activation
    # regardless of what lockdownd returns. SBLockdownManager caches the
    # activation state code in ivar [self+4] and the brick flag in [self+9]
    # (filled by _setupActivationState from lockdown_copy_value). The two public
    # accessors just return those ivars; force their results:
    #   -[SBLockdownManager lockdownState] (vaddr 0x7474c):
    #       0x74770  ldr r3,[r4,#4]  e5943004  ->  mov r3,#1  e3a03001  (Activated)
    #   -[SBLockdownManager brickedDevice] (vaddr 0x7471c):
    #       0x74740  ldrsb r0,[r4,#9] e1d400d9 ->  mov r0,#0  e3a00000  (not bricked)
    # State code 1 == "Activated" (set via `str sl,[self+4]` with sl=1 when
    # lockdown reports "Activated"). These are leaf accessors, so forcing their
    # return values touches nothing else. vaddr = file_off + 0x1000.
    p.patch_bytes('/System/Library/CoreServices/SpringBoard.app/SpringBoard', [
        (0x73770, bytes.fromhex('043094e5'), bytes.fromhex('0130a0e3')),
        (0x73740, bytes.fromhex('d900d4e1'), bytes.fromhex('0000a0e3')),
    ])

    # The partition must end exactly at the HFS+ volume end: the kernel probes
    # the alternate volume header at partition end - 1024, so trailing padding
    # in the raw image (or an off-by-one end LBA) points it at garbage.
    sig, = struct.unpack_from('>H', buf, 1024)
    assert sig in (0x482B, 0x4858), 'no HFS+/HFSX volume header at 1024'
    bs, total = struct.unpack_from('>II', buf, 1024 + 40)
    vol = bs * total
    assert len(buf) >= vol and vol % LBA == 0, (len(buf), vol)
    del buf[vol:]
    if buf[vol - 1024:vol - 512] != buf[1024:1536]:
        print(f'alternate volume header rewritten at {vol - 1024:#x}')
        buf[vol - 1024:vol - 512] = buf[1024:1536]
    with open(out, 'wb') as f:
        f.write(disk_header(len(buf) // LBA))
        f.write(buf)
    print(f'{out}: {PART_LBA * LBA + len(buf)} bytes, HFS+ at LBA {PART_LBA} '
          f'({len(buf) // LBA} LBAs) = FTL vpn {206848 + PART_LBA}')


if __name__ == '__main__':
    main()
