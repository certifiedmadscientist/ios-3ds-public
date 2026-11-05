#!/bin/bash
# Host replay of SPI2 transfers through periph.c + pl080.c + input.c + sysic.c.
set -euo pipefail
HERE=$(cd "$(dirname "$0")" && pwd); BOOT=$HERE/../../boot; WORK=$(mktemp -d)
trap 'rm -rf "$WORK"' EXIT
F="-O1 -w -ffreestanding -fno-builtin -I$BOOT -ffunction-sections -fdata-sections"
for f in periph pl080 input sysic; do cc $F -c "$BOOT/periph/$f.c" -o "$WORK/$f.o"; done
cc -O1 -w -o "$WORK/h" "$HERE/harness.c" "$WORK"/*.o -Wl,--gc-sections
"$WORK/h"
