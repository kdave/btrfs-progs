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

#include <stdio.h>
#include <stdlib.h>
#include <errno.h>
#include <string.h>
#include <endian.h>
#include <byteswap.h>
#include <assert.h>
#include <stddef.h>
#include <linux/types.h>
#include <stdint.h>

#include <features.h>

#ifndef __GLIBC__
#ifndef BTRFS_DISABLE_BACKTRACE
#define BTRFS_DISABLE_BACKTRACE
#endif
#define __always_inline __inline __attribute__ ((__always_inline__))
#endif

#ifndef BTRFS_DISABLE_BACKTRACE
#include <execinfo.h>
#endif

#define ptr_to_u64(x)	((u64)(uintptr_t)x)
#define u64_to_ptr(x)	((void *)(uintptr_t)x)

#ifndef READ
#define READ 0
#define WRITE 1
#define READA 2
#endif

#define gfp_t int
#define get_cpu_var(p) (p)
#define __get_cpu_var(p) (p)
#define BITS_PER_BYTE 8
#define BITS_PER_LONG (__SIZEOF_LONG__ * BITS_PER_BYTE)
#define __GFP_BITS_SHIFT 20
#define __GFP_BITS_MASK ((int)((1 << __GFP_BITS_SHIFT) - 1))
#define GFP_KERNEL 0
#define GFP_NOFS 0
#define __read_mostly
#define ARRAY_SIZE(x) (sizeof(x) / sizeof((x)[0]))

#ifndef ULONG_MAX
#define ULONG_MAX       (~0UL)
#endif

#define __token_glue(a,b,c)	___token_glue(a,b,c)
#define ___token_glue(a,b,c)	a ## b ## c
#ifdef DEBUG_BUILD_CHECKS
#define BUILD_ASSERT(x)		extern int __token_glue(compile_time_assert_,__LINE__,__COUNTER__)[1-2*!(x)] __attribute__((unused))
#else
#define BUILD_ASSERT(x)
#endif

#ifndef BTRFS_DISABLE_BACKTRACE
#define MAX_BACKTRACE	16
static inline void print_trace(void)
{
	void *array[MAX_BACKTRACE];
	int size;

	size = backtrace(array, MAX_BACKTRACE);
	backtrace_symbols_fd(array, size, 2);
}
#endif

static inline void warning_trace(const char *assertion, const char *filename,
			      const char *func, unsigned line, long val)
{
	if (!val)
		return;
	fprintf(stderr,
		"%s:%d: %s: Warning: assertion `%s` failed, value %ld\n",
		filename, line, func, assertion, val);
#ifndef BTRFS_DISABLE_BACKTRACE
	print_trace();
#endif
}

static inline void bugon_trace(const char *assertion, const char *filename,
			      const char *func, unsigned line, long val)
{
	if (!val)
		return;
	fprintf(stderr,
		"%s:%d: %s: BUG_ON `%s` triggered, value %ld\n",
		filename, line, func, assertion, val);
#ifndef BTRFS_DISABLE_BACKTRACE
	print_trace();
#endif
	abort();
	exit(1);
}

#ifdef __CHECKER__
#define __force    __attribute__((force))
#define __bitwise__ __attribute__((bitwise))
#else
#define __force
#define __bitwise__
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

struct mutex {
	unsigned long lock;
};

#define mutex_init(m)						\
do {								\
	(m)->lock = 1;						\
} while (0)

static inline void mutex_lock(struct mutex *m)
{
	m->lock--;
}

static inline void mutex_unlock(struct mutex *m)
{
	m->lock++;
}

static inline int mutex_is_locked(struct mutex *m)
{
	return (m->lock != 1);
}

#define cond_resched()		do { } while (0)
#define preempt_enable()	do { } while (0)
#define preempt_disable()	do { } while (0)

#define BITOP_MASK(nr)		(1UL << ((nr) % BITS_PER_LONG))
#define BITOP_WORD(nr)		((nr) / BITS_PER_LONG)

#ifndef __attribute_const__
#define __attribute_const__	__attribute__((__const__))
#endif

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

/*
 * This looks more complex than it should be. But we need to
 * get the type for the ~ right in round_down (it needs to be
 * as wide as the result!), and we want to evaluate the macro
 * arguments just once each.
 */
#define __round_mask(x, y) ((__typeof__(x))((y)-1))
#define round_up(x, y) ((((x)-1) | __round_mask(x, y))+1)
#define round_down(x, y) ((x) & ~__round_mask(x, y))

/*
 * printk
 */
#define printk(fmt, args...) fprintf(stderr, fmt, ##args)
#define	KERN_CRIT	""
#define KERN_ERR	""

/*
 * kmalloc/kfree
 */
#define kmalloc(x, y) malloc(x)
#define kzalloc(x, y) calloc(1, x)
#define kstrdup(x, y) strdup(x)
#define kfree(x) free(x)
#define vmalloc(x) malloc(x)
#define vfree(x) free(x)

#ifndef BTRFS_DISABLE_BACKTRACE
static inline void assert_trace(const char *assertion, const char *filename,
			      const char *func, unsigned line, long val)
{
	if (val)
		return;
	fprintf(stderr,
		"%s:%d: %s: Assertion `%s` failed, value %ld\n",
		filename, line, func, assertion, val);
#ifndef BTRFS_DISABLE_BACKTRACE
	print_trace();
#endif
	abort();
	exit(1);
}
#define	ASSERT(c) assert_trace(#c, __FILE__, __func__, __LINE__, (long)(c))
#else
#define ASSERT(c) assert(c)
#endif

#define BUG_ON(c) bugon_trace(#c, __FILE__, __func__, __LINE__, (long)(c))
#define BUG() BUG_ON(1)
#define WARN_ON(c) warning_trace(#c, __FILE__, __func__, __LINE__, (long)(c))

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

/* Alignment check */
#define IS_ALIGNED(x, a)                (((x) & ((typeof(x))(a) - 1)) == 0)

static inline int is_power_of_2(unsigned long n)
{
	return (n != 0 && ((n & (n - 1)) == 0));
}

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

#ifndef true
#define true 1
#define false 0
#endif

#ifndef noinline
#define noinline
#endif

#endif
