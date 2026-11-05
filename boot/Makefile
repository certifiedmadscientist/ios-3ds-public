CC = arm-none-eabi-gcc
AS = arm-none-eabi-gcc
LD = arm-none-eabi-ld
OBJCOPY = arm-none-eabi-objcopy

ARCH = -mcpu=mpcore -march=armv6k -mfloat-abi=soft

# Inputs generated from the user's IPSW by ../build.sh (none of them are in
# the source tree): the patched kernel (for the ksyms backtrace table), the
# flattened n45ap DeviceTree (embedded in the payload) and Key 0x837 (the
# emulated AES engine's GID stand-in), 32 hex digits.
KERNEL ?= ../out/sd/KERNEL.BIN
DEVICETREE ?= ../out/work/n45ap.devicetree
KEY837 ?=
# Root filesystem: nand = emulated NAND backed by SD:/DISK.IMG (rd=disk0s1),
# md = the RAMDISK.DMG ramdisk (rd=md0).  make clean when switching.
ROOT ?= nand
ROOT_NAND = $(if $(filter nand,$(ROOT)),1,0)
# Panel the kernel is told about: 3ds = 320x240 (the bottom LCD, mirrored
# 1:1), iphone = the real 320x480 (see ORIENT).
LCD ?= 3ds
LCD_H = $(if $(filter iphone,$(LCD)),480,240)
# With LCD=iphone: portrait = the panel rotated and area-filtered into a
# 240x320 portrait view (hold the 3DS like a book, top screen on the
# left); landscape = the old 2:1 squish.  FIT=aspect keeps the aspect ratio
# (2/3 scale, 213 px wide with side bars), FIT=fill stretches the width to
# 240.  make clean when switching.
ORIENT ?= portrait
FIT ?= aspect
PORTRAIT = $(if $(and $(filter iphone,$(LCD)),$(filter portrait,$(ORIENT))),1,0)
PORTRAIT_FILL = $(if $(filter fill,$(FIT)),1,0)

CFLAGS = -std=gnu11 -ffreestanding -nostdlib -O2 $(ARCH) -Wall -Wextra -Wno-unused-parameter -MMD -MP -DROOT_NAND=$(ROOT_NAND) -DLCD_H=$(LCD_H) -DPORTRAIT=$(PORTRAIT) -DPORTRAIT_FILL=$(PORTRAIT_FILL)
ASFLAGS = $(ARCH) -MMD -MP
LIBGCC = $(shell $(CC) $(ARCH) -print-libgcc-file-name)
LDFLAGS = -T link.ld -nostdlib

PERIPH_SRC = $(wildcard periph/*.c)
PERIPH_OBJ = $(PERIPH_SRC:.c=.o)

OBJS = start.o abort.o kern_htl.o main.o screen.o console.o mmu.o dtree.o dtblob.o loader.o bootargs.o htl.o kps.o ksyms.o display.o portrait.o pm.o $(PERIPH_OBJ)

all: arm9_build payload.firm

boot.elf: $(OBJS)
	$(LD) $(LDFLAGS) -o $@ $^ $(LIBGCC)

dtblob.o: n45ap.devicetree

n45ap.devicetree: $(DEVICETREE)
	cp $< $@

# Kernel symbol table for kps.c backtraces, generated from the kernel image.
ksyms.S: $(KERNEL) ../tools/mk_ksyms.py
	python3 -I ../tools/mk_ksyms.py $< $@

# Key 0x837 as a C initializer list.  Rewritten only when the value changes.
key837.inc: FORCE
	@python3 -I -c 'import sys; k = bytes.fromhex(sys.argv[1]); assert len(k) == 16; print(",".join("0x%02X" % b for b in k))' '$(KEY837)' > $@.tmp 2>/dev/null \
		|| { rm -f $@.tmp; echo "KEY837 must be set to Key 0x837 (32 hex digits)"; exit 1; }
	@cmp -s $@.tmp $@ && rm -f $@.tmp || mv $@.tmp $@

periph/aes.o: key837.inc

-include $(OBJS:.o=.d)

%.o: %.S
	$(AS) $(ASFLAGS) -c -o $@ $<

%.o: %.c
	$(CC) $(CFLAGS) -c -o $@ $<

payload.bin: boot.elf
	$(OBJCOPY) -O binary $< $@

stub11.bin: stub11.o
	$(OBJCOPY) -O binary $< $@

arm9_build:
	$(MAKE) -C arm9

payload.firm: payload.bin stub11.bin arm9_build
	python3 ../tools/mkfirm.py \
		--arm11-bin payload.bin --arm11-addr 0x21800000 \
		--stub11-bin stub11.bin --stub11-addr 0x1FF80000 \
		--arm9-bin arm9/arm9.bin --arm9-addr 0x08000000 \
		--output $@

periph/%.o: periph/%.c
	$(CC) $(CFLAGS) -c -o $@ $<

clean:
	rm -f ksyms.S n45ap.devicetree key837.inc *.o *.d periph/*.o periph/*.d boot.elf payload.bin stub11.bin payload.firm
	$(MAKE) -C arm9 clean

.PHONY: all clean arm9_build FORCE
