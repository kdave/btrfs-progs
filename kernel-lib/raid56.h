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

#endif
