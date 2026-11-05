#ifndef PM_H
#define PM_H

#include "types.h"

/* System sleep on Hold (pm.c). */
bool  pm_patch_kernel(void);                      /* before the kernel runs */
u32   htl_kern_undef_hook(u32 *frame, u32 spsr);  /* kern_htl.S undef entry */
bool  pm_asleep(void);
char *pm_status(char *p);                         /* " P<s><req><sleeps>" */
void  pm_trace_line(char *buf);                   /* "PM ..." status row */

#endif
