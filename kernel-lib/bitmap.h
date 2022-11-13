/* SPDX-License-Identifier: GPL-2.0 */

/*
 * A user-space bitmap wrapper to provide a subset of kernel bitmap operations.
 *
 * Most functions are not a direct copy of the kernel version, but should be
 * good enough for single thread usage.
 */

#ifndef _BTRFS_PROGS_LINUX_BITMAP_H_
#define _BTRFS_PROGS_LINUX_BITMAP_H_

#include <stdlib.h>
#include "kerncompat.h"
#include "kernel-lib/bitops.h"

static inline unsigned long *bitmap_zalloc(unsigned int nbits)
{
	return calloc(BITS_TO_LONGS(nbits), BITS_PER_LONG);
}

static inline void bitmap_free(unsigned long *bitmap)
{
	free(bitmap);
}

#define BITMAP_LAST_WORK_MASK(nbits) (~0ULL >> (-(nbits) & (BITS_PER_LONG - 1)))

static inline unsigned int bitmap_weight(const unsigned long *bitmap, unsigned int nbits)
{
	int ret = 0;
	int i;

	/* Handle the aligned part first. */
	for (i = 0; i < nbits / BITS_PER_LONG; i++)
		ret += hweight_long(bitmap[i]);

	/* The remaining unaligned part. */
	if (nbits % BITS_PER_LONG)
		ret += bitmap[i] & BITMAP_LAST_WORD_MASK(nbits);

	return ret;
}

#endif
