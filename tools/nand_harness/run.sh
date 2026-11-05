#!/bin/bash
# Build and run the NAND emulation harness.
#
#   run.sh DISK.IMG [GEN_SRC]
#
# GEN_SRC: a checkout of devos50/qemu-ios-generate-nand at branch main
# (the iPod touch 1G layout).  When given, the generator is run on the
# first 8 MB of the HFS+ volume in DISK.IMG and its pages are compared
# byte-for-byte with what the emulation synthesizes.
set -euo pipefail

DISK=$(realpath "$1")
GEN=${2:+$(realpath "$2")}
HERE=$(cd "$(dirname "$0")" && pwd)
BOOT=$HERE/../../boot
WORK=$(mktemp -d)
trap 'rm -rf "$WORK"' EXIT

cc -O2 -Wall -Wno-unused-parameter -ffreestanding -fno-builtin -I"$BOOT" \
   -c "$BOOT/periph/nand.c" -o "$WORK/nand.o"
cc -O2 -Wall -Wno-unused-parameter -ffreestanding -fno-builtin -I"$BOOT" \
   -c "$BOOT/periph/pl080.c" -o "$WORK/pl080.o"
cc -O2 -Wall -o "$WORK/harness" "$HERE/harness.c" "$HERE/hal_host.c" \
   "$WORK/nand.o" "$WORK/pl080.o"

GENDIR=
if [ -n "$GEN" ]; then
    (cd "$WORK" && cc -O2 -w -I"$GEN" "$GEN/generate_nand.c" -o generate_nand &&
     dd if="$DISK" of=filesystem-readonly.img bs=2048 skip=3 count=4096 status=none &&
     ./generate_nand > generate.log)
    GENDIR=$WORK/nand
fi

"$WORK/harness" "$DISK" $GENDIR
