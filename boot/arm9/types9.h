/*
 * types9.h -- Basic type definitions for ARM9 bare-metal code
 */

#ifndef TYPES9_H
#define TYPES9_H

typedef unsigned char       u8;
typedef unsigned short      u16;
typedef unsigned int        u32;
typedef unsigned long long  u64;

typedef signed char         s8;
typedef signed short        s16;
typedef signed int          s32;
typedef signed long long    s64;

typedef volatile u8         vu8;
typedef volatile u16        vu16;
typedef volatile u32        vu32;
typedef volatile u64        vu64;

typedef u32                 size_t;
typedef s32                 ssize_t;
typedef u32                 uintptr_t;

#ifndef __cplusplus
#include <stdbool.h>
#endif

#define NULL ((void *)0)

#define PACKED   __attribute__((packed))
#define ALIGNED(n) __attribute__((aligned(n)))
#define NORETURN __attribute__((noreturn))
#define UNUSED   __attribute__((unused))

#endif /* TYPES9_H */
