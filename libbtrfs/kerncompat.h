/*
 * Copyright (C) 2007 Oracle.  All rights reserved.
 *
 * This program is free software; you can redistribute it and/or
 * modify it under the terms of the GNU General Public
 * License v2 as published by the Free Software Foundation.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * General Public License for more details.
 *
 * You should have received a copy of the GNU General Public
 * License along with this program; if not, write to the
 * Free Software Foundation, Inc., 59 Temple Place - Suite 330,
 * Boston, MA 021110-1307, USA.
 */

#ifndef __KERNCOMPAT_H__
#define __KERNCOMPAT_H__

#ifndef __SANE_USERSPACE_TYPES__
/* For PPC64 to get LL64 types */
#define __SANE_USERSPACE_TYPES__
#endif

#include <stdio.h>
#include <stdlib.h>
#include <endian.h>
#include <byteswap.h>
#include <stddef.h>

/*
 * Libc compatibility.
 */
#if !defined(__GLIBC__) || defined(__UCLIBC__)
/* Musl does not define __always_inline */
#ifndef __always_inline
#define __always_inline __inline __attribute__ ((__always_inline__))
#endif
#endif

#ifdef __CHECKER__
#define __force    __attribute__((force))
#define __bitwise__ __attribute__((bitwise))
#else
#define __force
#ifndef __bitwise__
#define __bitwise__
#endif
#endif

#ifndef __CHECKER__
/*
 * Since we're using primitive definitions from kernel-space, we need to
 * define __KERNEL__ so that system header files know which definitions
 * to use.
 */
#define __KERNEL__
#include <asm/types.h>
typedef __u32 u32;
typedef __u64 u64;
typedef __u16 u16;
typedef __u8 u8;
typedef __s64 s64;
typedef __s32 s32;

/*
 * Continuing to define __KERNEL__ breaks others parts of the code, so
 * we can just undefine it now that we have the correct headers...
 */
#undef __KERNEL__
#else
typedef unsigned int u32;
typedef unsigned int __u32;
typedef unsigned long long u64;
typedef unsigned char u8;
typedef unsigned short u16;
typedef long long s64;
typedef int s32;
#endif

/*
 * error pointer
 */
#define MAX_ERRNO	4095
#define IS_ERR_VALUE(x) ((x) >= (unsigned long)-MAX_ERRNO)

static inline void *ERR_PTR(long error)
{
	return (void *) error;
}

static inline long PTR_ERR(const void *ptr)
{
	return (long) ptr;
}

static inline int IS_ERR(const void *ptr)
{
	return IS_ERR_VALUE((unsigned long)ptr);
}

static inline int IS_ERR_OR_NULL(const void *ptr)
{
	return !ptr || IS_ERR(ptr);
}

#define BUG()								\
do {									\
	fprintf(stderr, "%s:%u: BUG in %s()\n", __FILE__, __LINE__, __func__);	\
	abort();							\
	exit(1);							\
	__builtin_unreachable();					\
} while (0)

#define container_of(ptr, type, member) ({                      \
        const typeof( ((type *)0)->member ) *__mptr = (ptr);    \
	        (type *)( (char *)__mptr - offsetof(type,member) );})
#ifndef __bitwise
#ifdef __CHECKER__
#define __bitwise __bitwise__
#else
#define __bitwise
#endif /* __CHECKER__ */
#endif	/* __bitwise */

typedef u16 __bitwise __le16;
typedef u16 __bitwise __be16;
typedef u32 __bitwise __le32;
typedef u32 __bitwise __be32;
typedef u64 __bitwise __le64;
typedef u64 __bitwise __be64;

/* Macros to generate set/get funcs for the struct fields
 * assume there is a lefoo_to_cpu for every type, so lets make a simple
 * one for u8:
 */
#define le8_to_cpu(v) (v)
#define cpu_to_le8(v) (v)
#define __le8 u8

#if __BYTE_ORDER == __BIG_ENDIAN
#define cpu_to_le64(x) ((__force __le64)(u64)(bswap_64(x)))
#define le64_to_cpu(x) ((__force u64)(__le64)(bswap_64(x)))
#define cpu_to_le32(x) ((__force __le32)(u32)(bswap_32(x)))
#define le32_to_cpu(x) ((__force u32)(__le32)(bswap_32(x)))
#define cpu_to_le16(x) ((__force __le16)(u16)(bswap_16(x)))
#define le16_to_cpu(x) ((__force u16)(__le16)(bswap_16(x)))
#else
#define cpu_to_le64(x) ((__force __le64)(u64)(x))
#define le64_to_cpu(x) ((__force u64)(__le64)(x))
#define cpu_to_le32(x) ((__force __le32)(u32)(x))
#define le32_to_cpu(x) ((__force u32)(__le32)(x))
#define cpu_to_le16(x) ((__force __le16)(u16)(x))
#define le16_to_cpu(x) ((__force u16)(__le16)(x))
#endif

struct __una_u16 { __le16 x; } __attribute__((__packed__));
struct __una_u32 { __le32 x; } __attribute__((__packed__));
struct __una_u64 { __le64 x; } __attribute__((__packed__));

#define get_unaligned_le8(p) (*((u8 *)(p)))
#define get_unaligned_8(p) (*((u8 *)(p)))
#define put_unaligned_le8(val,p) ((*((u8 *)(p))) = (val))
#define put_unaligned_8(val,p) ((*((u8 *)(p))) = (val))
#define get_unaligned_le16(p) le16_to_cpu(((const struct __una_u16 *)(p))->x)
#define get_unaligned_16(p) (((const struct __una_u16 *)(p))->x)
#define put_unaligned_le16(val,p) (((struct __una_u16 *)(p))->x = cpu_to_le16(val))
#define put_unaligned_16(val,p) (((struct __una_u16 *)(p))->x = (val))
#define get_unaligned_le32(p) le32_to_cpu(((const struct __una_u32 *)(p))->x)
#define get_unaligned_32(p) (((const struct __una_u32 *)(p))->x)
#define put_unaligned_le32(val,p) (((struct __una_u32 *)(p))->x = cpu_to_le32(val))
#define put_unaligned_32(val,p) (((struct __una_u32 *)(p))->x = (val))
#define get_unaligned_le64(p) le64_to_cpu(((const struct __una_u64 *)(p))->x)
#define get_unaligned_64(p) (((const struct __una_u64 *)(p))->x)
#define put_unaligned_le64(val,p) (((struct __una_u64 *)(p))->x = cpu_to_le64(val))
#define put_unaligned_64(val,p) (((struct __una_u64 *)(p))->x = (val))

/*
 * Note: simplified versions of READ_ONCE and WRITE_ONCE for source
 * compatibility only, not usable for lock-less implementation like in kernel.
 *
 * Changed:
 * - __unqual_scalar_typeof: volatile cast to typeof()
 * - compiletime_assert_rwonce_type: no word size compatibility checks
 * - no const volatile cast
 */

#define READ_ONCE(x)		(x)

#define WRITE_ONCE(x, val)						\
do {									\
	(x) = (val);							\
} while (0)

#endif
