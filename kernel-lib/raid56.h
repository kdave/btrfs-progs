/*
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

/*
 * Original headers from kernel library for RAID5/6 calculations, not from
 * btrfs kernel header.
 */

#ifndef __BTRFS_PROGS_RAID56_H__
#define __BTRFS_PROGS_RAID56_H__

void raid6_gen_syndrome(int disks, size_t bytes, void **ptrs);
int raid5_gen_result(int nr_devs, size_t stripe_len, int dest, void **data);

/*
 * Headers synchronized from kernel include/linux/raid/pq.h
 * No modification at all.
 *
 * Galois field tables.
 */
extern const u8 raid6_gfmul[256][256] __attribute__((aligned(256)));
extern const u8 raid6_vgfmul[256][32] __attribute__((aligned(256)));
extern const u8 raid6_gfexp[256]      __attribute__((aligned(256)));
extern const u8 raid6_gfinv[256]      __attribute__((aligned(256)));
extern const u8 raid6_gfexi[256]      __attribute__((aligned(256)));

/* Recover raid6 with 2 data stripes corrupted */
int raid6_recov_data2(int nr_devs, size_t stripe_len, int dest1, int dest2,
		      void **data);

/* Recover data and P */
int raid6_recov_datap(int nr_devs, size_t stripe_len, int dest1, void **data);

/*
 * Recover raid56 data
 * @dest1/2 can be -1 to indicate correct data
 *
 * Return >0 for unrecoverable case.
 * Return 0 for recoverable case, And recovered data will be stored into @data
 * Return <0 for fatal error
 */
int raid56_recov(int nr_devs, size_t stripe_len, u64 profile, int dest1,
		 int dest2, void **data);

#endif
