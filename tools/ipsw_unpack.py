#!/usr/bin/env python3
"""Pull the three inputs the port needs out of an iPod touch 1G IPSW.

  ipsw_unpack.py IPSW OUTDIR

Reads Restore.plist to find the release kernelcache and the root filesystem
DMG, and takes the n45ap DeviceTree from Firmware/all_flash.  Writes them to
OUTDIR and prints shell assignments for build.sh:

  KERNELCACHE=...  DEVICETREE_IMG2=...  ROOTFS_DMG=...

Only iPod1,1 iPhone OS 1.1.5 (4B1) has been tested; other builds are
rejected unless IOS3DS_ANY_BUILD=1 is set (patch offsets are build-specific).
"""
import os
import plistlib
import shlex
import shutil
import sys
import zipfile


def main():
    if len(sys.argv) != 3:
        sys.exit(__doc__)
    ipsw, out = sys.argv[1], sys.argv[2]
    os.makedirs(out, exist_ok=True)
    try:
        z = zipfile.ZipFile(ipsw)
    except (OSError, zipfile.BadZipFile) as e:
        sys.exit(f'{ipsw}: not a readable IPSW (zip): {e}')
    names = z.namelist()
    rp = plistlib.loads(z.read('Restore.plist'))
    build = (rp.get('ProductType'), rp.get('ProductVersion'), rp.get('ProductBuildVersion'))
    print(f'IPSW: {build[0]} {build[1]} ({build[2]})', file=sys.stderr)
    if build != ('iPod1,1', '1.1.5', '4B1') and os.environ.get('IOS3DS_ANY_BUILD') != '1':
        sys.exit('this port is built against iPod1,1 1.1.5 (4B1) only')

    kc = rp['RestoreKernelCaches']['Release']
    rootfs = rp['SystemRestoreImages']['User']
    dt = [n for n in names if os.path.basename(n) == 'DeviceTree.n45ap.img2']
    if len(dt) != 1:
        sys.exit('DeviceTree.n45ap.img2 not found in the IPSW')

    result = {}
    for var, member in (('KERNELCACHE', kc), ('DEVICETREE_IMG2', dt[0]), ('ROOTFS_DMG', rootfs)):
        dst = os.path.join(out, os.path.basename(member))
        with z.open(member) as src, open(dst, 'wb') as f:
            shutil.copyfileobj(src, f, 1 << 20)
        print(f'  {member} -> {dst}', file=sys.stderr)
        result[var] = os.path.abspath(dst)
    for var, path in result.items():
        print(f'{var}={shlex.quote(path)}')


if __name__ == '__main__':
    main()
