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

#ifndef __KERNCOMPAT
#define __KERNCOMPAT

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <endian.h>
#include <byteswap.h>

#define gfp_t int
#define get_cpu_var(p) (p)
#define __get_cpu_var(p) (p)
#define BITS_PER_LONG (sizeof(long) * 8)
#define __GFP_BITS_SHIFT 20
#define __GFP_BITS_MASK ((int)((1 << __GFP_BITS_SHIFT) - 1))
#define GFP_KERNEL 0
#define GFP_NOFS 0
#define __read_mostly
#define ARRAY_SIZE(x) (sizeof(x) / sizeof((x)[0]))
#define ULONG_MAX       (~0UL)
#define BUG() abort()
#ifdef __CHECKER__
#define __force    __attribute__((force))
#define __bitwise__ __attribute__((bitwise))
#else
#define __force
#define __bitwise__
#endif

#ifndef __CHECKER__
#include <asm/types.h>
typedef __u32 u32;
typedef __u64 u64;
typedef __u16 u16;
typedef __u8 u8;
#else
typedef unsigned int u32;
typedef unsigned int __u32;
typedef unsigned long long u64;
typedef unsigned char u8;
typedef unsigned short u16;
#endif


struct vma_shared { int prio_tree_node; };
struct vm_area_struct {
	unsigned long vm_pgoff;
	unsigned long vm_start;
	unsigned long vm_end;
	struct vma_shared shared;
};
struct page {
	unsigned long index;
};

#define preempt_enable()	do { } while (0)
#define preempt_disable()	do { } while (0)

#define BITOP_MASK(nr)		(1UL << ((nr) % BITS_PER_LONG))
#define BITOP_WORD(nr)		((nr) / BITS_PER_LONG)

/**
 * __set_bit - Set a bit in memory
 * @nr: the bit to set
 * @addr: the address to start counting from
 *
 * Unlike set_bit(), this function is non-atomic and may be reordered.
 * If it's called on the same region of memory simultaneously, the effect
 * may be that only one operation succeeds.
 */
static inline void __set_bit(int nr, volatile unsigned long *addr)
{
	unsigned long mask = BITOP_MASK(nr);
	unsigned long *p = ((unsigned long *)addr) + BITOP_WORD(nr);

	*p  |= mask;
}

static inline void __clear_bit(int nr, volatile unsigned long *addr)
{
	unsigned long mask = BITOP_MASK(nr);
	unsigned long *p = ((unsigned long *)addr) + BITOP_WORD(nr);

	*p &= ~mask;
}

/**
 * test_bit - Determine whether a bit is set
 * @nr: bit number to test
 * @addr: Address to start counting from
 */
static inline int test_bit(int nr, const volatile unsigned long *addr)
{
	return 1UL & (addr[BITOP_WORD(nr)] >> (nr & (BITS_PER_LONG-1)));
}

#define BUG_ON(c) do { if (c) abort(); } while (0)

#undef offsetof
#ifdef __compiler_offsetof
#define offsetof(TYPE,MEMBER) __compiler_offsetof(TYPE,MEMBER)
#else
#define offsetof(TYPE, MEMBER) ((size_t) &((TYPE *)0)->MEMBER)
#endif

#define container_of(ptr, type, member) ({                      \
        const typeof( ((type *)0)->member ) *__mptr = (ptr);    \
	        (type *)( (char *)__mptr - offsetof(type,member) );})

#define ENOMEM 5
#define EEXIST 6

#ifdef __CHECKER__
#define __CHECK_ENDIAN__
#define __bitwise __bitwise__
#else
#define __bitwise
#endif

typedef u16 __bitwise __le16;
typedef u16 __bitwise __be16;
typedef u32 __bitwise __le32;
typedef u32 __bitwise __be32;
typedef u64 __bitwise __le64;
typedef u64 __bitwise __be64;

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
#endif
