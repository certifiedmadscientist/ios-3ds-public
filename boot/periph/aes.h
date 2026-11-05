#ifndef AES_H
#define AES_H

#include "../types.h"

void aes_init(void);
u32  aes_read(u32 off);
void aes_write(u32 off, u32 value);

#endif /* AES_H */
