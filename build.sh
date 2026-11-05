#!/usr/bin/env bash
# Build everything the 3DS needs from a user-supplied iPhone OS 1.1.5 IPSW.
#
#   ./build.sh [options] iPod1,1_1.1.5_4B1_Restore.ipsw
#
# Options:
#   --keys FILE        read KEY837= and ROOTFS_KEY= from FILE
#                      (default: ./keys.conf if it exists)
#   --key837 HEX       S5L8900 Key 0x837, 32 hex digits (kernelcache and
#                      DeviceTree are 8900/IMG2 files encrypted with it)
#   --rootfs-key HEX   VFDecrypt key of the root filesystem DMG, 72 hex digits
#   --lcd iphone|3ds   panel mode (default iphone: portrait 320x480 view)
#   --out DIR          output directory (default ./out)
#   --keep-work        keep the intermediate files in OUT/work
#   -j N               parallel make jobs (default: number of CPUs)
#
# Keys can also come from the environment (KEY837, ROOTFS_KEY).  No key is
# stored in this repository; see README.md for where to find them.
#
# Everything runs on the host and only writes inside OUT and boot/.
set -euo pipefail

ROOT=$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)
OUT=$ROOT/out
LCD=iphone
KEEP=0
KEYS=
JOBS=$(nproc 2>/dev/null || echo 2)
IPSW=
ARG_KEY837=
ARG_ROOTFS_KEY=

die() { echo "build.sh: $*" >&2; exit 1; }
usage() { sed -n '2,20p' "$0" | sed 's/^# \{0,1\}//'; exit "${1:-0}"; }

while [ $# -gt 0 ]; do
    case $1 in
        --keys)       KEYS=$2; shift 2 ;;
        --key837)     ARG_KEY837=$2; shift 2 ;;
        --rootfs-key) ARG_ROOTFS_KEY=$2; shift 2 ;;
        --lcd)        LCD=$2; shift 2 ;;
        --out)        OUT=$2; shift 2 ;;
        --keep-work)  KEEP=1; shift ;;
        -j)           JOBS=$2; shift 2 ;;
        -j*)          JOBS=${1#-j}; shift ;;
        -h|--help)    usage 0 ;;
        -*)           die "unknown option $1" ;;
        *)            [ -z "$IPSW" ] || die "more than one IPSW given"; IPSW=$1; shift ;;
    esac
done
[ -n "$IPSW" ] || usage 1
[ -f "$IPSW" ] || die "no such file: $IPSW"
case $LCD in iphone|3ds) ;; *) die "--lcd must be iphone or 3ds" ;; esac

# Keys: config file < environment < command line.
if [ -z "$KEYS" ] && [ -f "$ROOT/keys.conf" ]; then KEYS=$ROOT/keys.conf; fi
if [ -n "$KEYS" ]; then
    [ -f "$KEYS" ] || die "no such keys file: $KEYS"
    while IFS='=' read -r k v; do
        k=$(echo "$k" | tr -d '[:space:]'); v=$(echo "${v%%#*}" | tr -d '[:space:]"'"'")
        case $k in
            KEY837)     : "${KEY837:=$v}" ;;
            ROOTFS_KEY) : "${ROOTFS_KEY:=$v}" ;;
        esac
    done < <(grep -E '^[[:space:]]*(KEY837|ROOTFS_KEY)[[:space:]]*=' "$KEYS")
fi
KEY837=${ARG_KEY837:-${KEY837:-}}
ROOTFS_KEY=${ARG_ROOTFS_KEY:-${ROOTFS_KEY:-}}
[[ $KEY837 =~ ^[0-9a-fA-F]{32}$ ]] || die "Key 0x837 missing or not 32 hex digits (--key837 or KEY837 in keys.conf)"
[[ $ROOTFS_KEY =~ ^[0-9a-fA-F]{72}$ ]] || die "rootfs key missing or not 72 hex digits (--rootfs-key or ROOTFS_KEY in keys.conf)"

# Host tools.
for t in python3 make arm-none-eabi-gcc arm-none-eabi-ld arm-none-eabi-objcopy; do
    command -v "$t" >/dev/null || die "missing host tool: $t"
done
python3 -c 'import cryptography' 2>/dev/null || python3 -c 'import Crypto' 2>/dev/null ||
    die "missing Python package: cryptography (pip install cryptography)"

IPSW=$(cd "$(dirname "$IPSW")" && pwd)/$(basename "$IPSW")
mkdir -p "$OUT"
OUT=$(cd "$OUT" && pwd)
WORK=$OUT/work
SD=$OUT/sd
rm -rf "$WORK" "$SD"
mkdir -p "$WORK" "$SD/luma/payloads"
PY="python3 -I"
T=$ROOT/tools
step() { printf '\n==> %s\n' "$*"; }

step "Unpacking the IPSW"
eval "$($PY "$T/ipsw_unpack.py" "$IPSW" "$WORK/ipsw")"

step "Kernel: decrypt, decompress, patch"
$PY "$T/extract_kernelcache.py" --key837 "$KEY837" "$KERNELCACHE" "$WORK/kernelcache.macho" > "$WORK/extract_kernelcache.log" ||
    { cat "$WORK/extract_kernelcache.log"; die "kernelcache extraction failed (wrong Key 0x837?)"; }
$PY "$T/patch_kernel.py" "$ROOT/patches/kernel.patches" "$WORK/kernelcache.macho" "$SD/KERNEL.BIN"

step "DeviceTree"
$PY "$T/extract_devicetree.py" --key837 "$KEY837" "$DEVICETREE_IMG2" "$WORK/n45ap.devicetree"

step "Root filesystem: decrypt the DMG"
$PY "$T/dmg_extract.py" --key "$ROOTFS_KEY" "$ROOTFS_DMG" "$WORK/rootfs.img"
rm -f "$ROOTFS_DMG"

step "NAND disk image (fstab, LaunchDaemons, activation patches)"
$PY "$T/mk_nand_disk.py" "$WORK/rootfs.img" "$SD/DISK.IMG"
[ "$KEEP" = 1 ] || rm -f "$WORK/rootfs.img"

step "FIRM payload (LCD=$LCD)"
make -s -C "$ROOT/boot" clean
make -s -C "$ROOT/boot" -j"$JOBS" LCD="$LCD" \
    KERNEL="$SD/KERNEL.BIN" DEVICETREE="$WORK/n45ap.devicetree" KEY837="$KEY837"
cp "$ROOT/boot/payload.firm" "$SD/luma/payloads/ios3ds.firm"

[ "$KEEP" = 1 ] || rm -rf "$WORK"

step "Done"
( cd "$SD" && python3 -I -c 'import hashlib, sys
for f in sys.argv[1:]:
    h = hashlib.md5()
    with open(f, "rb") as fp:
        for b in iter(lambda: fp.read(1 << 20), b""):
            h.update(b)
    print(h.hexdigest(), f)' KERNEL.BIN DISK.IMG luma/payloads/ios3ds.firm )
cat <<EOF

Copy these files to the root of the 3DS SD card (FAT32), keeping the paths:

  $SD/KERNEL.BIN                  ->  SD:/KERNEL.BIN
  $SD/DISK.IMG                    ->  SD:/DISK.IMG
  $SD/luma/payloads/ios3ds.firm   ->  SD:/luma/payloads/ios3ds.firm

Do not put a RAMDISK.DMG in the SD root.  Then hold START while powering on
the 3DS and pick ios3ds.firm in the Luma3DS chainloader menu.
EOF
