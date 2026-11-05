#ifndef DISPLAY_H
#define DISPLAY_H

#include "types.h"

u32  clcd_read(u32 off);
void clcd_write(u32 off, u32 value);

void display_mirror_step(void);
void display_status_line(char *buf);
void display_boot_pattern(void);
void display_sleep_blank(void);             /* bottom screen black now */
u32  htl_fb_trap(const u32 *args);

#endif
