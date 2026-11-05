/*
 * dtree.h — Apple Device Tree for iOS 3DS bootloader
 *
 * The kernel gets Apple's real n45ap tree (iPod touch 1G), embedded from
 * the IPSW, with unsupported devices left out and iBoot's placeholder
 * properties filled in.
 *
 * Apple DT node layout:
 *   struct { uint32_t num_properties; uint32_t num_children; }
 *   followed by properties inline, then child nodes inline.
 *
 * Apple DT property layout:
 *   char name[32];      // null-terminated, zero-padded to 32 bytes
 *   uint32_t length;    // byte count of value data
 *   uint8_t value[];    // padded to 4-byte alignment
 */

#ifndef DTREE_H
#define DTREE_H

#include "types.h"

/* Build the tree into buffer.  ramdisk_addr is a kernel VA.
 * Returns the tree size in bytes, or 0 on failure. */
size_t dtree_build_n45ap(void *buffer, size_t max_size, u32 mem_size,
                         u32 ramdisk_addr, u32 ramdisk_size);

#endif /* DTREE_H */
