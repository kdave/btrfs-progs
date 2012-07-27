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

#ifndef __UTILS__
#define __UTILS__

#define BTRFS_MKFS_SYSTEM_GROUP_SIZE (4 * 1024 * 1024)

int make_btrfs(int fd, const char *device, const char *label,
	       u64 blocks[6], u64 num_bytes, u32 nodesize,
	       u32 leafsize, u32 sectorsize, u32 stripesize);
int btrfs_make_root_dir(struct btrfs_trans_handle *trans,
			struct btrfs_root *root, u64 objectid);
int btrfs_prepare_device(int fd, char *file, int zero_end, u64 *block_count_ret,
			 u64 max_block_count, int *mixed, int nodiscard);
int btrfs_add_to_fsid(struct btrfs_trans_handle *trans,
		      struct btrfs_root *root, int fd, char *path,
		      u64 block_count, u32 io_width, u32 io_align,
		      u32 sectorsize);
int btrfs_scan_for_fsid(struct btrfs_fs_devices *fs_devices, u64 total_devs,
			int run_ioctls);
void btrfs_register_one_device(char *fname);
int btrfs_scan_one_dir(char *dirname, int run_ioctl);
int check_mounted(const char *devicename);
int check_mounted_where(int fd, const char *file, char *where, int size,
			struct btrfs_fs_devices **fs_devices_mnt);
int btrfs_device_already_in_root(struct btrfs_root *root, int fd,
				 int super_offset);
char *pretty_sizes(u64 size);
int check_label(char *input);
int get_mountpt(char *dev, char *mntpt, size_t size);

int btrfs_scan_block_devices(int run_ioctl);
#endif
