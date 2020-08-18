/* -*- linux-c -*- ------------------------------------------------------- *
 *
 *   Copyright 2002-2004 H. Peter Anvin - All Rights Reserved
 *
 *   This program is free software; you can redistribute it and/or modify
 *   it under the terms of the GNU General Public License as published by
 *   the Free Software Foundation, Inc., 53 Temple Place Ste 330,
 *   Boston MA 02111-1307, USA; either version 2 of the License, or
 *   (at your option) any later version; incorporated herein by reference.
 *
 * ----------------------------------------------------------------------- */

/*
 * Added helpers for unaligned native int access
 */

/*
 * raid6int1.c
 *
 * 1-way unrolled portable integer math RAID-6 instruction set
 *
 * This file was postprocessed using unroll.pl and then ported to userspace
 */
#include <stdint.h>
#include <unistd.h>
#include "kerncompat.h"
#include "kernel-shared/ctree.h"
#include "kernel-shared/disk-io.h"
#include "kernel-shared/volumes.h"
#include "common/utils.h"
#include "kernel-lib/raid56.h"

/*
 * This is the C data type to use
 */

/* Change this from BITS_PER_LONG if there is something better... */
#if BITS_PER_LONG == 64
# define NBYTES(x) ((x) * 0x0101010101010101UL)
# define NSIZE  8
# define NSHIFT 3
typedef uint64_t unative_t;
#define put_unaligned_native(val,p)	put_unaligned_64((val),(p))
#define get_unaligned_native(p)		get_unaligned_64((p))
#else
# define NBYTES(x) ((x) * 0x01010101U)
# define NSIZE  4
# define NSHIFT 2
typedef uint32_t unative_t;
#define put_unaligned_native(val,p)	put_unaligned_32((val),(p))
#define get_unaligned_native(p)		get_unaligned_32((p))
#endif

/*
 * These sub-operations are separate inlines since they can sometimes be
 * specially optimized using architecture-specific hacks.
 */

/*
 * The SHLBYTE() operation shifts each byte left by 1, *not*
 * rolling over into the next byte
 */
static inline __attribute_const__ unative_t SHLBYTE(unative_t v)
{
	unative_t vv;

	vv = (v << 1) & NBYTES(0xfe);
	return vv;
}

/*
 * The MASK() operation returns 0xFF in any byte for which the high
 * bit is 1, 0x00 for any byte for which the high bit is 0.
 */
static inline __attribute_const__ unative_t MASK(unative_t v)
{
	unative_t vv;

	vv = v & NBYTES(0x80);
	vv = (vv << 1) - (vv >> 7); /* Overflow on the top bit is OK */
	return vv;
}


void raid6_gen_syndrome(int disks, size_t bytes, void **ptrs)
{
	uint8_t **dptr = (uint8_t **)ptrs;
	uint8_t *p, *q;
	int d, z, z0;

	unative_t wd0, wq0, wp0, w10, w20;

	z0 = disks - 3;		/* Highest data disk */
	p = dptr[z0+1];		/* XOR parity */
	q = dptr[z0+2];		/* RS syndrome */

	for ( d = 0 ; d < bytes ; d += NSIZE*1 ) {
		wq0 = wp0 = get_unaligned_native(&dptr[z0][d+0*NSIZE]);
		for ( z = z0-1 ; z >= 0 ; z-- ) {
			wd0 = get_unaligned_native(&dptr[z][d+0*NSIZE]);
			wp0 ^= wd0;
			w20 = MASK(wq0);
			w10 = SHLBYTE(wq0);
			w20 &= NBYTES(0x1d);
			w10 ^= w20;
			wq0 = w10 ^ wd0;
		}
		put_unaligned_native(wp0, &p[d+NSIZE*0]);
		put_unaligned_native(wq0, &q[d+NSIZE*0]);
	}
}

static void xor_range(char *dst, const char*src, size_t size)
{
	/* Move to DWORD aligned */
	while (size && ((unsigned long)dst & sizeof(unsigned long))) {
		*dst++ ^= *src++;
		size--;
	}

	/* DWORD aligned part */
	while (size >= sizeof(unsigned long)) {
		*(unsigned long *)dst ^= *(unsigned long *)src;
		src += sizeof(unsigned long);
		dst += sizeof(unsigned long);
		size -= sizeof(unsigned long);
	}
	/* Remaining */
	while (size) {
		*dst++ ^= *src++;
		size--;
	}
}

/*
 * Generate desired data/parity stripe for RAID5
 *
 * @nr_devs:	Total number of devices, including parity
 * @stripe_len:	Stripe length
 * @data:	Data, with special layout:
 * 		data[0]:	 Data stripe 0
 * 		data[nr_devs-2]: Last data stripe
 * 		data[nr_devs-1]: RAID5 parity
 * @dest:	To generate which data. should follow above data layout
 */
int raid5_gen_result(int nr_devs, size_t stripe_len, int dest, void **data)
{
	int i;
	char *buf = data[dest];

	/* Validation check */
	if (stripe_len <= 0 || stripe_len != BTRFS_STRIPE_LEN) {
		error("invalid parameter for %s", __func__);
		return -EINVAL;
	}

	if (dest >= nr_devs || nr_devs < 2) {
		error("invalid parameter for %s", __func__);
		return -EINVAL;
	}
	/* Shortcut for 2 devs RAID5, which is just RAID1 */
	if (nr_devs == 2) {
		memcpy(data[dest], data[1 - dest], stripe_len);
		return 0;
	}
	memset(buf, 0, stripe_len);
	for (i = 0; i < nr_devs; i++) {
		if (i == dest)
			continue;
		xor_range(buf, data[i], stripe_len);
	}
	return 0;
}

/*
 * Raid 6 recovery code copied from kernel lib/raid6/recov.c.
 * With modifications:
 * - rename from raid6_2data_recov_intx1
 * - kfree/free modification for btrfs-progs
 */
int raid6_recov_data2(int nr_devs, size_t stripe_len, int dest1, int dest2,
		      void **data)
{
	u8 *p, *q, *dp, *dq;
	u8 px, qx, db;
	const u8 *pbmul;	/* P multiplier table for B data */
	const u8 *qmul;		/* Q multiplier table (for both) */
	char *zero_mem1, *zero_mem2;
	int ret = 0;

	/* Early check */
	if (dest1 < 0 || dest1 >= nr_devs - 2 ||
	    dest2 < 0 || dest2 >= nr_devs - 2 || dest1 >= dest2)
		return -EINVAL;

	zero_mem1 = calloc(1, stripe_len);
	zero_mem2 = calloc(1, stripe_len);
	if (!zero_mem1 || !zero_mem2) {
		free(zero_mem1);
		free(zero_mem2);
		return -ENOMEM;
	}

	p = (u8 *)data[nr_devs - 2];
	q = (u8 *)data[nr_devs - 1];

	/* Compute syndrome with zero for the missing data pages
	   Use the dead data pages as temporary storage for
	   delta p and delta q */
	dp = (u8 *)data[dest1];
	data[dest1] = (void *)zero_mem1;
	data[nr_devs - 2] = dp;
	dq = (u8 *)data[dest2];
	data[dest2] = (void *)zero_mem2;
	data[nr_devs - 1] = dq;

	raid6_gen_syndrome(nr_devs, stripe_len, data);

	/* Restore pointer table */
	data[dest1]   = dp;
	data[dest2]   = dq;
	data[nr_devs - 2] = p;
	data[nr_devs - 1] = q;

	/* Now, pick the proper data tables */
	pbmul = raid6_gfmul[raid6_gfexi[dest2 - dest1]];
	qmul  = raid6_gfmul[raid6_gfinv[raid6_gfexp[dest1]^raid6_gfexp[dest2]]];

	/* Now do it... */
	while ( stripe_len-- ) {
		px    = *p ^ *dp;
		qx    = qmul[*q ^ *dq];
		*dq++ = db = pbmul[px] ^ qx; /* Reconstructed B */
		*dp++ = db ^ px; /* Reconstructed A */
		p++; q++;
	}

	free(zero_mem1);
	free(zero_mem2);
	return ret;
}

/*
 * Raid 6 recover code copied from kernel lib/raid6/recov.c
 * - rename from raid6_datap_recov_intx1()
 * - parameter changed from faila to dest1
 */
int raid6_recov_datap(int nr_devs, size_t stripe_len, int dest1, void **data)
{
	u8 *p, *q, *dq;
	const u8 *qmul;		/* Q multiplier table */
	char *zero_mem;

	p = (u8 *)data[nr_devs - 2];
	q = (u8 *)data[nr_devs - 1];

	zero_mem = calloc(1, stripe_len);
	if (!zero_mem)
		return -ENOMEM;

	/* Compute syndrome with zero for the missing data page
	   Use the dead data page as temporary storage for delta q */
	dq = (u8 *)data[dest1];
	data[dest1] = (void *)zero_mem;
	data[nr_devs - 1] = dq;

	raid6_gen_syndrome(nr_devs, stripe_len, data);

	/* Restore pointer table */
	data[dest1]   = dq;
	data[nr_devs - 1] = q;

	/* Now, pick the proper data tables */
	qmul  = raid6_gfmul[raid6_gfinv[raid6_gfexp[dest1]]];

	/* Now do it... */
	while ( stripe_len-- ) {
		*p++ ^= *dq = qmul[*q ^ *dq];
		q++; dq++;
	}
	return 0;
}

/* Original raid56 recovery wrapper */
int raid56_recov(int nr_devs, size_t stripe_len, u64 profile, int dest1,
		 int dest2, void **data)
{
	int min_devs;
	int ret;

	if (profile & BTRFS_BLOCK_GROUP_RAID5)
		min_devs = 2;
	else if (profile & BTRFS_BLOCK_GROUP_RAID6)
		min_devs = 3;
	else
		return -EINVAL;
	if (nr_devs < min_devs)
		return -EINVAL;

	/* Nothing to recover */
	if (dest1 == -1 && dest2 == -1)
		return 0;

	/* Reorder dest1/2, so only dest2 can be -1  */
	if (dest1 == -1) {
		dest1 = dest2;
		dest2 = -1;
	} else if (dest2 != -1 && dest1 != -1) {
		/* Reorder dest1/2, ensure dest2 > dest1 */
		if (dest1 > dest2) {
			int tmp;

			tmp = dest2;
			dest2 = dest1;
			dest1 = tmp;
		}
	}

	if (profile & BTRFS_BLOCK_GROUP_RAID5) {
		if (dest2 != -1)
			return 1;
		return raid5_gen_result(nr_devs, stripe_len, dest1, data);
	}

	/* RAID6 one dev corrupted case*/
	if (dest2 == -1) {
		/* Regenerate P/Q */
		if (dest1 == nr_devs - 1 || dest1 == nr_devs - 2) {
			raid6_gen_syndrome(nr_devs, stripe_len, data);
			return 0;
		}

		/* Regenerate data from P */
		return raid5_gen_result(nr_devs - 1, stripe_len, dest1, data);
	}

	/* P/Q bot corrupted */
	if (dest1 == nr_devs - 2 && dest2 == nr_devs - 1) {
		raid6_gen_syndrome(nr_devs, stripe_len, data);
		return 0;
	}

	/* 2 Data corrupted */
	if (dest2 < nr_devs - 2)
		return raid6_recov_data2(nr_devs, stripe_len, dest1, dest2,
					 data);
	/* Data and P*/
	if (dest2 == nr_devs - 1)
		return raid6_recov_datap(nr_devs, stripe_len, dest1, data);

	/*
	 * Final case, Data and Q, recover data first then regenerate Q
	 */
	ret = raid5_gen_result(nr_devs - 1, stripe_len, dest1, data);
	if (ret < 0)
		return ret;
	raid6_gen_syndrome(nr_devs, stripe_len, data);
	return 0;
}
