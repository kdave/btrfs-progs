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
#include <errno.h>
#include <string.h>
#include <endian.h>
#include <byteswap.h>
#include <assert.h>
#include <stddef.h>
#include <linux/types.h>
#include <linux/const.h>
#include <stdint.h>
#include <stdbool.h>
#include <stdarg.h>
#include <features.h>

/*
 * Glibc supports backtrace, some other libc implementations don't but need to
 * be more careful detecting proper glibc.
 */
#if !defined(__GLIBC__) || defined(__UCLIBC__)
#ifndef BTRFS_DISABLE_BACKTRACE
#define BTRFS_DISABLE_BACKTRACE
#endif

#ifndef __always_inline
#define __always_inline __inline __attribute__ ((__always_inline__))
#endif

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
#define __GFP_DMA32 0
#define __GFP_HIGHMEM 0
#define GFP_KERNEL 0
#define GFP_NOFS 0
#define GFP_NOWAIT 0
#define GFP_ATOMIC 0
#define __read_mostly
#define ARRAY_SIZE(x) (sizeof(x) / sizeof((x)[0]))
#define _RET_IP_ 0
#define TASK_UNINTERRUPTIBLE 0
#define SLAB_MEM_SPREAD 0
#define ALLOW_ERROR_INJECTION(a, b)

#ifndef ULONG_MAX
#define ULONG_MAX       (~0UL)
#endif

#ifndef SECTOR_SHIFT
#define SECTOR_SHIFT	(9)
#endif

#define __token_glue(a,b,c)	___token_glue(a,b,c)
#define ___token_glue(a,b,c)	a ## b ## c
#ifdef DEBUG_BUILD_CHECKS
#define BUILD_ASSERT(x)		extern int __token_glue(compile_time_assert_,__LINE__,__COUNTER__)[1-2*!(x)] __attribute__((unused))
#else
#define BUILD_ASSERT(x)
#endif

static inline void print_trace(void)
{
#ifndef BTRFS_DISABLE_BACKTRACE
#define MAX_BACKTRACE	16
	void *array[MAX_BACKTRACE];
	int size;

	size = backtrace(array, MAX_BACKTRACE);
	backtrace_symbols_fd(array, size, 2);
#endif
}

static inline void warning_trace(const char *assertion, const char *filename,
			      const char *func, unsigned line, long val)
{
	if (!val)
		return;
	fprintf(stderr,
		"%s:%u: %s: Warning: assertion `%s` failed, value %ld\n",
		filename, line, func, assertion, val);
	print_trace();
}

static inline void bugon_trace(const char *assertion, const char *filename,
			      const char *func, unsigned line, long val)
{
	if (!val)
		return;
	fprintf(stderr,
		"%s:%u: %s: BUG_ON `%s` triggered, value %ld\n",
		filename, line, func, assertion, val);
	print_trace();
	abort();
	exit(1);
}

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

typedef u64 sector_t;

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

typedef struct spinlock_struct {
	unsigned long lock;
} spinlock_t;

struct rw_semaphore {
	long lock;
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

static inline void spin_lock_init(spinlock_t *lock)
{
	lock->lock = 0;
}

static inline void spin_lock(spinlock_t *lock)
{
	lock->lock++;
}

static inline void spin_unlock(spinlock_t *lock)
{
	lock->lock--;
}

#define spin_lock_irqsave(_l, _f) do { _f = 0; spin_lock((_l)); } while (0)

static inline void spin_unlock_irqrestore(spinlock_t *lock, unsigned long flags)
{
	spin_unlock(lock);
}

static inline void init_rwsem(struct rw_semaphore *sem)
{
	sem->lock = 0;
}

static inline bool down_read_trylock(struct rw_semaphore *sem)
{
	sem->lock++;
	return true;
}

static inline void down_read(struct rw_semaphore *sem)
{
	sem->lock++;
}

static inline void up_read(struct rw_semaphore *sem)
{
	sem->lock--;
}

#define cond_resched()		do { } while (0)
#define preempt_enable()	do { } while (0)
#define preempt_disable()	do { } while (0)
#define might_sleep()		do { } while (0)

#define BITOP_MASK(nr)		(1UL << ((nr) % BITS_PER_LONG))
#define BITOP_WORD(nr)		((nr) / BITS_PER_LONG)

#ifndef __attribute_const__
#define __attribute_const__	__attribute__((__const__))
#endif

/* To silence compilers (like clang) that don't understand fallthrough comments. */

#if defined __has_attribute
# if __has_attribute(__fallthrough__)
#  define fallthrough			__attribute__((__fallthrough__))
# endif
#else
# define fallthrough			do {} while (0)  /* fallthrough */
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

#define div_u64(x, y) ((x) / (y))

/**
 * __swap - swap values of @a and @b
 * @a: first value
 * @b: second value
 */
#define __swap(a, b) \
        do { typeof(a) __tmp = (a); (a) = (b); (b) = __tmp; } while (0)

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
#define KERN_EMERG	""
#define KERN_ALERT	""
#define KERN_CRIT	""
#define KERN_NOTICE	""
#define KERN_INFO	""
#define KERN_WARNING	""

/*
 * kmalloc/kfree
 */
#define kmalloc(x, y) malloc(x)
#define kzalloc(x, y) calloc(1, x)
#define kstrdup(x, y) strdup(x)
#define kfree(x) free(x)
#define vmalloc(x) malloc(x)
#define vfree(x) free(x)
#define kvzalloc(x, y) kzalloc(x,y)
#define kvfree(x) free(x)
#define memalloc_nofs_save() (0)
#define memalloc_nofs_restore(x)	((void)(x))
#define __releases(x)
#define __acquires(x)

struct kmem_cache {
	size_t size;
};

static inline struct kmem_cache *kmem_cache_create(const char *name,
						   size_t size, unsigned long idk,
						   unsigned long flags, void *private)
{
	struct kmem_cache *ret = malloc(sizeof(*ret));
	if (!ret)
		return ret;
	ret->size = size;
	return ret;
}

static inline void kmem_cache_destroy(struct kmem_cache *cache)
{
	free(cache);
}

static inline void *kmem_cache_alloc(struct kmem_cache *cache, gfp_t mask)
{
	return malloc(cache->size);
}

static inline void *kmem_cache_zalloc(struct kmem_cache *cache, gfp_t mask)
{
	return calloc(1, cache->size);
}

static inline void kmem_cache_free(struct kmem_cache *cache, void *ptr)
{
	free(ptr);
}

#define BUG_ON(c) bugon_trace(#c, __FILE__, __func__, __LINE__, (long)(c))
#define BUG()				\
do {					\
	BUG_ON(1);			\
	__builtin_unreachable();	\
} while (0)

#define WARN_ON(c) ({					\
	int __ret_warn_on = !!(c);			\
	warning_trace(#c, __FILE__, __func__, __LINE__,	\
		      (long)(__ret_warn_on));		\
	__ret_warn_on;					\
})

#define WARN(c, msg...) ({				\
	int __ret_warn_on = !!(c);			\
	if (__ret_warn_on)				\
		printf(msg);				\
	__ret_warn_on;					\
})

#define IS_ENABLED(c) 0

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

/*
 * Alignment, copied and renamed from /usr/include/linux/const.h to work around
 * issues caused by moving the definition in 5.12
 */
#define __ALIGN_KERNEL__(x, a)		__ALIGN_KERNEL_MASK__(x, (typeof(x))(a) - 1)
#define __ALIGN_KERNEL_MASK__(x, mask)	(((x) + (mask)) & ~(mask))
#define ALIGN(x, a)		__ALIGN_KERNEL__((x), (a))

static inline int is_power_of_2(unsigned long n)
{
	return (n != 0 && ((n & (n - 1)) == 0));
}

/**
 * const_ilog2 - log base 2 of 32-bit or a 64-bit constant unsigned value
 * @n: parameter
 *
 * Use this where sparse expects a true constant expression, e.g. for array
 * indices.
 */
#define const_ilog2(n)				\
(						\
	__builtin_constant_p(n) ? (		\
		(n) < 2 ? 0 :			\
		(n) & (1ULL << 63) ? 63 :	\
		(n) & (1ULL << 62) ? 62 :	\
		(n) & (1ULL << 61) ? 61 :	\
		(n) & (1ULL << 60) ? 60 :	\
		(n) & (1ULL << 59) ? 59 :	\
		(n) & (1ULL << 58) ? 58 :	\
		(n) & (1ULL << 57) ? 57 :	\
		(n) & (1ULL << 56) ? 56 :	\
		(n) & (1ULL << 55) ? 55 :	\
		(n) & (1ULL << 54) ? 54 :	\
		(n) & (1ULL << 53) ? 53 :	\
		(n) & (1ULL << 52) ? 52 :	\
		(n) & (1ULL << 51) ? 51 :	\
		(n) & (1ULL << 50) ? 50 :	\
		(n) & (1ULL << 49) ? 49 :	\
		(n) & (1ULL << 48) ? 48 :	\
		(n) & (1ULL << 47) ? 47 :	\
		(n) & (1ULL << 46) ? 46 :	\
		(n) & (1ULL << 45) ? 45 :	\
		(n) & (1ULL << 44) ? 44 :	\
		(n) & (1ULL << 43) ? 43 :	\
		(n) & (1ULL << 42) ? 42 :	\
		(n) & (1ULL << 41) ? 41 :	\
		(n) & (1ULL << 40) ? 40 :	\
		(n) & (1ULL << 39) ? 39 :	\
		(n) & (1ULL << 38) ? 38 :	\
		(n) & (1ULL << 37) ? 37 :	\
		(n) & (1ULL << 36) ? 36 :	\
		(n) & (1ULL << 35) ? 35 :	\
		(n) & (1ULL << 34) ? 34 :	\
		(n) & (1ULL << 33) ? 33 :	\
		(n) & (1ULL << 32) ? 32 :	\
		(n) & (1ULL << 31) ? 31 :	\
		(n) & (1ULL << 30) ? 30 :	\
		(n) & (1ULL << 29) ? 29 :	\
		(n) & (1ULL << 28) ? 28 :	\
		(n) & (1ULL << 27) ? 27 :	\
		(n) & (1ULL << 26) ? 26 :	\
		(n) & (1ULL << 25) ? 25 :	\
		(n) & (1ULL << 24) ? 24 :	\
		(n) & (1ULL << 23) ? 23 :	\
		(n) & (1ULL << 22) ? 22 :	\
		(n) & (1ULL << 21) ? 21 :	\
		(n) & (1ULL << 20) ? 20 :	\
		(n) & (1ULL << 19) ? 19 :	\
		(n) & (1ULL << 18) ? 18 :	\
		(n) & (1ULL << 17) ? 17 :	\
		(n) & (1ULL << 16) ? 16 :	\
		(n) & (1ULL << 15) ? 15 :	\
		(n) & (1ULL << 14) ? 14 :	\
		(n) & (1ULL << 13) ? 13 :	\
		(n) & (1ULL << 12) ? 12 :	\
		(n) & (1ULL << 11) ? 11 :	\
		(n) & (1ULL << 10) ? 10 :	\
		(n) & (1ULL <<  9) ?  9 :	\
		(n) & (1ULL <<  8) ?  8 :	\
		(n) & (1ULL <<  7) ?  7 :	\
		(n) & (1ULL <<  6) ?  6 :	\
		(n) & (1ULL <<  5) ?  5 :	\
		(n) & (1ULL <<  4) ?  4 :	\
		(n) & (1ULL <<  3) ?  3 :	\
		(n) & (1ULL <<  2) ?  2 :	\
		1) :				\
	-1)

static inline int ilog2(u64 num)
{
	int l = 0;

	num >>= 1;
	while (num) {
		l++;
		num >>= 1;
	}

	return l;
}

typedef u16 __bitwise __le16;
typedef u16 __bitwise __be16;
typedef u32 __bitwise __le32;
typedef u32 __bitwise __be32;
typedef u64 __bitwise __le64;
typedef u64 __bitwise __be64;

#define U64_MAX			UINT64_MAX
#define U32_MAX			UINT32_MAX

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

#define smp_rmb() do {} while (0)
#define smp_mb__before_atomic() do {} while (0)
#define smp_mb() do {} while (0)

struct percpu_counter {
	int count;
};

typedef struct refcount_struct {
	int refs;
} refcount_t;

static inline void refcount_set(refcount_t *ref, int val)
{
	ref->refs = val;
}

static inline void refcount_inc(refcount_t *ref)
{
	ref->refs++;
}

static inline void refcount_dec(refcount_t *ref)
{
	ref->refs--;
}

static inline bool refcount_dec_and_test(refcount_t *ref)
{
	ref->refs--;
	return ref->refs == 0;
}

typedef u32 blk_status_t;
typedef u32 blk_opf_t;
typedef int atomic_t;

struct work_struct;
typedef void (*work_func_t)(struct work_struct *work);

struct workqueue_struct {
};

struct work_struct {
	work_func_t func;
};

#define INIT_WORK(_w, _f) do { (_w)->func = (_f); } while (0)

typedef struct wait_queue_head {
} wait_queue_head_t;

struct wait_queue_entry {
};

#define DEFINE_WAIT(name)	struct wait_queue_entry name = {}

struct super_block {
	char *s_id;
};

struct va_format {
	const char *fmt;
	va_list *va;
};

struct lock_class_key {
};

#define __init
#define __cold
#define __user
#define __pure

#define __printf(a, b)                  __attribute__((__format__(printf, a, b)))

static inline bool sb_rdonly(struct super_block *sb)
{
	return false;
}

#define unlikely(cond) (cond)

#define rcu_dereference(c) (c)

#define rcu_assign_pointer(p, v) do { (p) = (v); } while (0)

static inline void atomic_set(atomic_t *a, int val)
{
	*a = val;
}

static inline int atomic_read(const atomic_t *a)
{
	return *a;
}

static inline void atomic_inc(atomic_t *a)
{
	(*a)++;
}

static inline void atomic_dec(atomic_t *a)
{
	(*a)--;
}

static inline bool atomic_inc_not_zero(atomic_t *a)
{
	if (*a) {
		atomic_inc(a);
		return true;
	}
	return false;
}

static inline struct workqueue_struct *alloc_workqueue(const char *name,
						       unsigned long flags,
						       int max_active, ...)
{
	return (struct workqueue_struct *)5;
}

static inline void destroy_workqueue(struct workqueue_struct *wq)
{
}

static inline void flush_workqueue(struct workqueue_struct *wq)
{
}

static inline void workqueue_set_max_active(struct workqueue_struct *wq,
					    int max_active)
{
}

static inline void queue_work(struct workqueue_struct *wq, struct work_struct *work)
{
}

static inline bool wq_has_sleeper(struct wait_queue_head *wq)
{
	return false;
}

static inline bool waitqueue_active(struct wait_queue_head *wq)
{
	return false;
}

static inline void wake_up(struct wait_queue_head *wq)
{
}

static inline void lockdep_set_class(spinlock_t *lock, struct lock_class_key *lclass)
{
}

static inline void lockdep_assert_held_read(struct rw_semaphore *sem)
{
}

static inline bool cond_resched_lock(spinlock_t *lock)
{
	return false;
}

static inline void init_waitqueue_head(wait_queue_head_t *wqh)
{
}

static inline bool need_resched(void)
{
	return false;
}

static inline bool gfpflags_allow_blocking(gfp_t mask)
{
	return true;
}

static inline void prepare_to_wait(wait_queue_head_t *wqh,
				   struct wait_queue_entry *entry,
				   unsigned long flags)
{
}

static inline void finish_wait(wait_queue_head_t *wqh,
			       struct wait_queue_entry *entry)
{
}

static inline void schedule(void)
{
}

static inline void rcu_read_lock(void)
{
}

static inline void rcu_read_unlock(void)
{
}

static inline void synchronize_rcu(void)
{
}

/*
 * Temporary definitions while syncing.
 */
struct btrfs_inode;
struct extent_state;
struct extent_buffer;
struct btrfs_root;
struct btrfs_trans_handle;

static inline void btrfs_merge_delalloc_extent(struct btrfs_inode *inode,
					       struct extent_state *state,
					       struct extent_state *other)
{
}

static inline void btrfs_set_delalloc_extent(struct btrfs_inode *inode,
					     struct extent_state *state,
					     u32 bits)
{
}

static inline void btrfs_split_delalloc_extent(struct btrfs_inode *inode,
					       struct extent_state *orig,
					       u64 split)
{
}

static inline void btrfs_clear_delalloc_extent(struct btrfs_inode *inode,
					       struct extent_state *state,
					       u32 bits)
{
}

static inline int btrfs_reloc_cow_block(struct btrfs_trans_handle *trans,
					struct btrfs_root *root,
					struct extent_buffer *buf,
					struct extent_buffer *cow)
{
	return 0;
}

static inline void btrfs_qgroup_trace_subtree_after_cow(struct btrfs_trans_handle *trans,
							struct btrfs_root *root,
							struct extent_buffer *buf)
{
}

#endif
