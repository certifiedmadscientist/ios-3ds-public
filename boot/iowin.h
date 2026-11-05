/*
 * iowin.h — where our runtime lives and how it reaches devices
 *
 * The whole payload image is linked at IMG_VA (inside the kernel's static
 * linear map, VA 0xC0000000+ -> PA 0x21000000+), so once XNU owns the MMU
 * our code and data stay mapped in every address space via TTBR1.  The
 * physical range 0x21663520-0x22FFFFFF lies below the kernel's avail_start
 * (~0x2362F000) and is never handed to the VM.
 *
 * Devices and memory outside kernel RAM are reached through a window at
 * 0x80000000-0x82FFFFFF.  No VM map covers it: user maps end at 0x40000000
 * (task_create_internal: vm_map_create(pmap, 0, 0x40000000), matching
 * TTBCR.N=2), kernel_map starts at 0xC0000000, and the only kernel_pmap
 * mapping in between is the commpage at 0x40000000.  The same window is
 * mapped in our pre-kernel tables (mmu_init) and kept in the kernel's
 * TTBR1 table by kern_map_window (kern_htl.S).
 */

#ifndef IOWIN_H
#define IOWIN_H

#include "types.h"

#define IMG_VA              0xC0800000u
#define IMG_PA              0x21800000u

#define IOWIN_IO_VA         0x80000000u     /* PA 0x10000000-0x10FFFFFF */
#define IOWIN_MP_VA         0x81000000u     /* PA 0x17E00000 (1 MB)     */
#define IOWIN_VRAM_VA       0x81800000u     /* PA 0x18000000-0x187FFFFF */
#define IOWIN_LOW_VA        0x82000000u     /* PA 0x20000000-0x20FFFFFF */

extern u32 iowin_on;

/* Kernel-window VA (0xC0000000+) of our image -> physical address. */
static inline u32 kva2pa(u32 va) {
    return va - 0xC0000000u + 0x21000000u;
}

/* Physical address of a device / framebuffer -> VA our code may use.
 * Identity until mmu_init has mapped the window. */
static inline u32 io_va(u32 pa) {
    if (!iowin_on)
        return pa;
    if (pa - 0x10000000u < 0x01000000u)
        return IOWIN_IO_VA + (pa - 0x10000000u);
    if (pa - 0x17E00000u < 0x00100000u)
        return IOWIN_MP_VA + (pa - 0x17E00000u);
    if (pa - 0x18000000u < 0x00800000u)
        return IOWIN_VRAM_VA + (pa - 0x18000000u);
    if (pa - 0x20000000u < 0x01000000u)
        return IOWIN_LOW_VA + (pa - 0x20000000u);
    if (pa - 0x21000000u < 0x07000000u)
        return pa + 0x9F000000u;            /* kernel linear map */
    return pa;
}

#define IOREG32(pa)  (*(volatile u32 *)io_va(pa))

#endif /* IOWIN_H */
