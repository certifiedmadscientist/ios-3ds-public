/*
 * loader.h — Mach-O kernelcache loader for iOS 3DS bootloader
 *
 * Parses a decrypted, decompressed 32-bit ARM Mach-O binary
 * (the iPhone OS 1.x kernelcache) and prepares it for execution.
 */

#ifndef LOADER_H
#define LOADER_H

#include "types.h"

/* ── Mach-O structures ──────────────────────────────────────────────── */

#define MH_MAGIC        0xFEEDFACE  /* 32-bit Mach-O */

#define CPU_TYPE_ARM    12
#define CPU_SUBTYPE_V6  6

#define MH_EXECUTE      2

#define LC_SEGMENT      0x01
#define LC_UNIXTHREAD   0x05

struct mach_header {
    u32 magic;
    u32 cputype;
    u32 cpusubtype;
    u32 filetype;
    u32 ncmds;
    u32 sizeofcmds;
    u32 flags;
} PACKED;

struct segment_command {
    u32  cmd;
    u32  cmdsize;
    char segname[16];
    u32  vmaddr;
    u32  vmsize;
    u32  fileoff;
    u32  filesize;
    u32  maxprot;
    u32  initprot;
    u32  nsects;
    u32  flags;
} PACKED;

/* LC_UNIXTHREAD carries an ARM thread-state blob:
 *   u32 flavor;    // 1 = ARM_THREAD_STATE
 *   u32 count;     // number of u32 words following
 *   u32 regs[17];  // r0-r12, sp, lr, pc, cpsr
 * The entry point is regs[15] (PC). */

/* ── Loader results ─────────────────────────────────────────────────── */

typedef struct {
    u32 entry_point;        /* Kernel entry address (virtual) */
    u32 load_base;          /* Lowest vmaddr of loaded segments */
    u32 load_end;           /* Highest vmaddr + vmsize */
} kernel_info_t;

/* Parse a Mach-O binary at `data` (size bytes).
 * Fills info with entry point and load addresses.
 * Returns true on success. */
bool loader_parse_macho(const void *data, size_t size, kernel_info_t *info);

/* Load the Mach-O segments into their virtual addresses.
 * The MMU must be active so that the iPhone DRAM range is mapped.
 * Returns true on success. */
bool loader_load_segments(const void *data, size_t size);

/* Jump to the kernel.  Does not return.
 * entry = virtual entry point, boot_args = pointer to boot_args_t. */
NORETURN void loader_jump_to_kernel(u32 entry, void *boot_args);

/* Search the loaded kernel for its exception vector template and patch
 * the data abort and undefined instruction entries to point to our
 * kern_htl handlers.  Saves the kernel's original handler addresses
 * into kern_da_orig_addr / kern_undef_orig_addr.
 * Returns true if the template was found and patched. */
bool loader_patch_kernel_vectors(const kernel_info_t *kinfo);

#endif /* LOADER_H */
