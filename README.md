# iPhone OS 1.1.5 on the Old Nintendo 3DS

This runs Apple's own iPhone OS 1.1.5 for the first-generation iPod touch
(iPod1,1, board n45ap, build 4B1) on an Old 3DS: the stock XNU kernel and
the stock userland, up to SpringBoard. Touch works, and SpringBoard shows in
portrait on the bottom screen.

The 3DS has no S5L8900, so the payload runs the kernel on the ARM11 and
emulates the iPod's hardware around it: interrupt controller, timers, DMA,
NAND, I2C/PMU, SPI multitouch, the LCD controller and so on. The root
filesystem lives in a disk image on the SD card. The ARM9 serves it to the
emulated NAND.

This repository has no Apple code or data. You supply the IPSW, and
`build.sh` makes everything from it on your computer: the decrypted and
patched kernel, the device tree, the disk image and the payload.

## Requirements

**3DS**
- An **Old 3DS / 3DS XL / 2DS** with boot9strap and Luma3DS installed (see
  <https://3ds.hacks.guide>). It has only been tested on an Old 3DS. The New
  3DS is untested.
- An SD card formatted FAT32 with about 300 MB free.

**Host** (Linux or macOS. Any system with these tools should work.)
- `bash`, `make`, Python 3.8 or later
- The Python package `cryptography` (`pip install cryptography`).
  PyCryptodome also works.
- An `arm-none-eabi` GCC toolchain (`arm-none-eabi-gcc`, `-ld`, `-objcopy`).
  You can use devkitARM or your distribution's package.

**Inputs**
- `iPod1,1_1.1.5_4B1_Restore.ipsw`, Apple's restore image for the iPod touch
  1G, iPhone OS 1.1.5. The tested file has md5
  `80b46fe8db7cfe58bb67e8553f495a8d`.
- Two keys. See [Keys](#keys).

## Keys

No keys ship with this repository. `build.sh` needs two of them:

| Name | What it decrypts | Format |
|------|------------------|--------|
| `KEY837` | The S5L8900 "Key 0x837". It decrypts the 8900/IMG2-wrapped `kernelcache.release.s5l8900xrb` and `DeviceTree.n45ap.img2`. The payload's emulated AES engine also uses it as the GID-key stand-in. | 32 hex digits |
| `ROOTFS_KEY` | The VFDecrypt key of the root filesystem `018-3755-1.dmg` | 72 hex digits |

Both keys are published on The Apple Wiki:
- `ROOTFS_KEY`: the firmware keys page for **Little Bear 4B1 (iPod1,1)**. Find
  it from <https://theapplewiki.com/wiki/Firmware_Keys/1.x>, under iPod touch.
- `KEY837`: the **Key 0x837** entry on
  <https://theapplewiki.com/wiki/AES_Keys>.

Pass the keys on the command line, or copy `keys.conf.example` to `keys.conf`
and fill it in. git ignores `keys.conf`.

## Build

```sh
cp keys.conf.example keys.conf      # fill in KEY837 and ROOTFS_KEY
./build.sh path/to/iPod1,1_1.1.5_4B1_Restore.ipsw
# or: ./build.sh --key837 <hex> --rootfs-key <hex> path/to/...ipsw
```

The build takes a few seconds and writes `out/sd/`. These are its steps:

1. **Unpack the IPSW** (`tools/ipsw_unpack.py`). It reads `Restore.plist`
   and takes the release kernelcache, `DeviceTree.n45ap.img2` and the root
   filesystem DMG. It rejects other builds, because every patch offset is
   specific to 4B1.
2. **Kernel** (`tools/extract_kernelcache.py`, `tools/patch_kernel.py`).
   It strips the 8900 header, decrypts with Key 0x837, unpacks IMG2 and
   decompresses LZSS to get the Mach-O. Then it applies
   `patches/kernel.patches`, a commented list of offsets with original and
   replacement bytes. Every original byte is checked first, and so are the
   md5 of the stock Mach-O and the md5 of the result. The result is
   `KERNEL.BIN`.
3. **Device tree** (`tools/extract_devicetree.py`). Apple's flattened n45ap
   DeviceTree, decrypted from the IPSW. The payload embeds it, and at boot
   `boot/dtree.c` fills in the values iBoot would normally set and adds the
   nodes the port needs.
4. **Root filesystem** (`tools/dmg_extract.py`). It decrypts the FileVault
   ("encrcdsa") layer, expands the UDIF image and keeps the HFS+ partition.
   It needs no external tools such as vfdecrypt or dmg2img.
5. **Disk image** (`tools/mk_nand_disk.py`). It wraps the volume in the
   MBR/GPT layout the iPod's NAND uses and edits the volume in place:
   - `fstab` mounts the root read-write;
   - SpringBoard uses software rendering;
   - LaunchDaemons the port can't support are disabled;
   - the activation bypass: four instruction patches in `lockdownd` and two
     in SpringBoard, each checked against the original bytes. The comments in
     the script explain them.

   The result is `DISK.IMG`.
6. **Payload** (`make -C boot LCD=iphone`). It builds the ARM11 program,
   the ARM11 start stub and the ARM9 SD block server, then packs them into a
   FIRM with `tools/mkfirm.py`. The kernel symbol table used for backtraces
   (`boot/ksyms.S`) is generated from `KERNEL.BIN` at build time.

Options: `--lcd 3ds` builds the older 320x240 landscape mode instead of the
portrait iPhone panel. Only the top half of SpringBoard shows in that mode.
`--out DIR` changes the output directory. `--keep-work` keeps the
intermediate files. To pass other panel options to the payload, such as
`FIT=fill` or `ORIENT=landscape`, run `make -C boot` yourself after a build.
The variables are described in `boot/Makefile`.

### Known-good outputs

| File | md5 |
|------|-----|
| `KERNEL.BIN` | `50bed0aaf1baf0ea266b852a447df6ac` |
| `DISK.IMG` | `7eff74ebb83450c25b75528ca91694ab` |
| `ios3ds.firm` (LCD=iphone) | `d13896af89e213b96453ff4ec119de71` (arm-none-eabi-gcc 16.1, so it may differ with other compilers) |

`build.sh` checks the kernel md5 itself. The DISK.IMG build is
deterministic, so the same IPSW gives the same md5.

## SD card layout

```
SD:/KERNEL.BIN                  patched kernelcache (out/sd/KERNEL.BIN)
SD:/DISK.IMG                    root filesystem disk (out/sd/DISK.IMG)
SD:/luma/payloads/ios3ds.firm   the payload
```

`KERNEL.BIN` and `DISK.IMG` must sit in the SD root with these exact
upper-case 8.3 names. The ARM9 loader reads the root directory only. Don't
leave a `RAMDISK.DMG` in the root. An older ramdisk boot mode used it, and
this build doesn't need it.

## Booting

1. Copy the three files to the SD card as shown above.
2. Hold **START** while powering on the 3DS. Luma3DS opens its chainloader
   menu. Pick `ios3ds`.
   To boot it with one button, rename the payload by Luma's convention, for
   example `x_ios3ds.firm` to boot when **X** is held.
3. The top screen shows the boot console: kernel log, peripheral status and
   progress rows. The bottom screen shows the iPod display once SpringBoard
   draws.

To get back to the 3DS, hold the power button to turn it off and boot
normally.

## Controls

Hold the 3DS like a book: turn it 90 degrees counter-clockwise, so the top
screen is on the left and the touch screen is on the right. The bottom screen
is then a portrait iPod display.

| 3DS | iPod |
|-----|------|
| Touch screen | Multitouch (one finger) |
| START or A | Home button |
| SELECT | Sleep/Wake (Hold) button |

## Known issues

- **Waking from the lock screen does not work.** After the screen turns off
  (Hold, or Auto-Lock), the display doesn't come back. Set **Settings ->
  General -> Auto-Lock -> Never** right after the first boot, and don't press
  SELECT.
- No audio and no Wi-Fi. The codec and the Wi-Fi chip aren't emulated.
- No battery or charging logic: the battery always reads full.
- Changes to the filesystem live in RAM only. `DISK.IMG` is read-only, so
  every boot starts fresh.
- Booting is slow, and SpringBoard renders in software (the MBX GPU is not
  emulated).

## Safety

The payload never touches the 3DS's internal NAND, and it doesn't write to
the SD card either. The ARM9 side opens `KERNEL.BIN`, `DISK.IMG` and the
optional `RAMDISK.DMG` read-only through its own small FAT32 reader. Writes
from iOS to its "NAND" go to a RAM overlay and are lost at power-off. The
host tools only write inside `out/` and `boot/`.

## Host test harnesses

These are host-side tests that compile pieces of the payload with the native
C compiler:

```sh
tools/portrait_test/run.sh                 # portrait scaler and touch mapping
tools/spi_harness/run.sh                   # SPI2 / PL080 / multitouch replay
tools/pmu_harness/run.sh [KERNEL.BIN]      # PMU battery model vs. the kext's curve
tools/nand_harness/run.sh out/sd/DISK.IMG  # emulated NAND / ADM / PL080 reads
```

`nand_harness/run.sh` can also compare against pages generated by
devos50/qemu-ios-generate-nand if you give it a checkout of that tool. The
`*_emu.py` scripts in that directory run the kernel's own NAND code under
Unicorn (`pip install unicorn`). They're debugging aids that the build
doesn't need.

## Layout

```
build.sh              single entry point
keys.conf.example     key template
patches/
  kernel.patches      kernel patch list (offsets + original/new bytes)
  fb_trap2.S          source of the trap -10 handler bytes in the list
boot/                 ARM11 payload: start-up, MMU, kernel loader, device
                      tree, hardware trap layer (htl.c, kern_htl.S, abort.S),
                      display/portrait, power management
boot/periph/          S5L8900 peripheral models
boot/arm9/            ARM9: SD (TMIO) driver, FAT32, kernel loader, block server
tools/                IPSW/kernel/rootfs tools, FIRM packer, harnesses
```

## Credits

Several peripheral models in `boot/periph/` follow, or are ported from,
devos50's qemu-ios (the iPod touch 1G emulator for QEMU) and its NAND
generator. The FIRM uses the public "sighax" boot9 signature.
