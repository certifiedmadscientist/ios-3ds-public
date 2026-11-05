#ifndef SHA1_H
#define SHA1_H

#include "../types.h"

void sha1_init(void);
u32  sha1_read(u32 off);
void sha1_write(u32 off, u32 value);

#endif /* SHA1_H */
