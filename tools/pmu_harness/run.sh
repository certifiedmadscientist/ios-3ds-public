#!/bin/bash
# Host check of the PMU battery model (periph/i2c.c + periph/pmu.c) against
# a C replay of ApplePCF50635PMUPowerSource's battery logic, using the
# battery-data curve taken from the prelinked kernel's kext Info.plist.
#
#   run.sh [KERNEL.BIN]      (default: out/sd/KERNEL.BIN from build.sh)
set -euo pipefail
HERE=$(cd "$(dirname "$0")" && pwd); ROOT=$HERE/../..; BOOT=$ROOT/boot; WORK=$(mktemp -d)
trap 'rm -rf "$WORK"' EXIT
python3 -I - "${1:-$ROOT/out/sd/KERNEL.BIN}" > "$WORK/curve.inc" <<'PY'
import base64, re, struct, sys
d = open(sys.argv[1], "rb").read()
i = d.index(b"<key>battery-data</key>")
m = re.search(rb"<key>0001-default</key><data[^>]*>([^<]+)</data>", d[i:i + 4096])
b = base64.b64decode(m.group(1))
w = struct.unpack("<%dI" % (len(b) // 4), b[:len(b) // 4 * 4])
print("static const u32 curve[] = {" + ", ".join("0x%x" % x for x in w) + "};")
PY
F="-O1 -w -ffreestanding -fno-builtin -I$BOOT"
for f in i2c pmu; do cc $F -c "$BOOT/periph/$f.c" -o "$WORK/$f.o"; done
cc -O1 -w -I"$WORK" -o "$WORK/h" "$HERE/harness.c" "$WORK"/*.o
"$WORK/h"
