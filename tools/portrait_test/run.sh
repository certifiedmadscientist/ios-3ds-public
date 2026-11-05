#!/bin/bash
# Host test of boot/portrait.c for both rotation directions and both fits.
set -euo pipefail
HERE=$(cd "$(dirname "$0")" && pwd); BOOT=$HERE/../../boot; WORK=$(mktemp -d)
trap 'rm -rf "$WORK"' EXIT
rc=0
for ccw in 1 0; do for fill in 0 1; do
    D="-DPORTRAIT_CCW=$ccw -DPORTRAIT_FILL=$fill"
    cc -O2 -w -ffreestanding -fno-builtin -I"$BOOT" $D -c "$BOOT/portrait.c" -o "$WORK/p.o"
    cc -O2 -w $D -o "$WORK/t" "$HERE/test.c" "$WORK/p.o"
    echo "== PORTRAIT_CCW=$ccw PORTRAIT_FILL=$fill"
    "$WORK/t" || rc=1
done; done
exit $rc
