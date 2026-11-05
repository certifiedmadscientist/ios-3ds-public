CC = arm-none-eabi-gcc
AS = arm-none-eabi-gcc
LD = arm-none-eabi-ld
OBJCOPY = arm-none-eabi-objcopy

# ARM946E-S is ARMv5TE
ARCH = -mcpu=arm946e-s -march=armv5te -mfloat-abi=soft
CFLAGS = -std=gnu11 -ffreestanding -nostdlib -O2 $(ARCH) -Wall -Wextra -Wno-unused-parameter
ASFLAGS = $(ARCH)
LIBGCC = $(shell $(CC) $(ARCH) -print-libgcc-file-name)
LDFLAGS = -T link9.ld -nostdlib

OBJS = start9.o main9.o tmio.o fat32.o blkserv.o

all: arm9.bin

arm9.elf: $(OBJS)
	$(LD) $(LDFLAGS) -o $@ $^ $(LIBGCC)

%.o: %.S
	$(AS) $(ASFLAGS) -c -o $@ $<

%.o: %.c
	$(CC) $(CFLAGS) -c -o $@ $<

arm9.bin: arm9.elf
	$(OBJCOPY) -O binary $< $@

clean:
	rm -f *.o arm9.elf arm9.bin
