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

#include <sys/stat.h>
#include "ctree.h"
#include <dirent.h>

#define BTRFS_MKFS_SYSTEM_GROUP_SIZE (4 * 1024 * 1024)

#define BTRFS_SCAN_PROC		(1ULL << 0)
#define BTRFS_SCAN_DEV		(1ULL << 1)
#define BTRFS_SCAN_MOUNTED	(1ULL << 2)
#define BTRFS_SCAN_LBLKID	(1ULL << 3)

#define BTRFS_UPDATE_KERNEL	1

#define BTRFS_ARG_UNKNOWN	0
#define BTRFS_ARG_MNTPOINT	1
#define BTRFS_ARG_UUID		2
#define BTRFS_ARG_BLKDEV	3

int make_btrfs(int fd, const char *device, const char *label,
	       u64 blocks[6], u64 num_bytes, u32 nodesize,
	       u32 leafsize, u32 sectorsize, u32 stripesize, u64 features);
int btrfs_make_root_dir(struct btrfs_trans_handle *trans,
			struct btrfs_root *root, u64 objectid);
int btrfs_prepare_device(int fd, char *file, int zero_end, u64 *block_count_ret,
			 u64 max_block_count, int *mixed, int discard);
int btrfs_add_to_fsid(struct btrfs_trans_handle *trans,
		      struct btrfs_root *root, int fd, char *path,
		      u64 block_count, u32 io_width, u32 io_align,
		      u32 sectorsize);
int btrfs_scan_for_fsid(int run_ioctls);
void btrfs_register_one_device(char *fname);
int btrfs_scan_one_dir(char *dirname, int run_ioctl);
int check_mounted(const char *devicename);
int check_mounted_where(int fd, const char *file, char *where, int size,
			struct btrfs_fs_devices **fs_devices_mnt);
int btrfs_device_already_in_root(struct btrfs_root *root, int fd,
				 int super_offset);

int pretty_size_snprintf(u64 size, char *str, size_t str_bytes);
#define pretty_size(size) 						\
	({								\
		static __thread char _str[24];				\
		(void)pretty_size_snprintf((size), _str, sizeof(_str));	\
		_str;							\
	})

int get_mountpt(char *dev, char *mntpt, size_t size);
int btrfs_scan_block_devices(int run_ioctl);
u64 parse_size(char *s);
int open_file_or_dir(const char *fname, DIR **dirstream);
void close_file_or_dir(int fd, DIR *dirstream);
int get_fs_info(char *path, struct btrfs_ioctl_fs_info_args *fi_args,
		struct btrfs_ioctl_dev_info_args **di_ret);
int get_label(const char *btrfs_dev, char *label);
int set_label(const char *btrfs_dev, const char *label);

char *__strncpy__null(char *dest, const char *src, size_t n);
int is_block_device(const char *file);
int is_mount_point(const char *file);
int open_path_or_dev_mnt(const char *path, DIR **dirstream);
u64 btrfs_device_size(int fd, struct stat *st);
/* Helper to always get proper size of the destination string */
#define strncpy_null(dest, src) __strncpy__null(dest, src, sizeof(dest))
int test_dev_for_mkfs(char *file, int force_overwrite, char *estr);
int scan_for_btrfs(int where, int update_kernel);
int get_label_mounted(const char *mount_path, char *labelp);
int test_num_disk_vs_raid(u64 metadata_profile, u64 data_profile,
	u64 dev_cnt, int mixed, char *estr);
int is_vol_small(char *file);
int csum_tree_block(struct btrfs_root *root, struct extent_buffer *buf,
			   int verify);
int ask_user(char *question);
int lookup_ino_rootid(int fd, u64 *rootid);
int btrfs_scan_lblkid(int update_kernel);
int get_btrfs_mount(const char *dev, char *mp, size_t mp_size);

#endif
