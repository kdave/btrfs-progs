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

#ifndef __BTRFS_UTILS_H__
#define __BTRFS_UTILS_H__

#include <sys/stat.h>
#include "ctree.h"
#include <dirent.h>

#define BTRFS_MKFS_SYSTEM_GROUP_SIZE (4 * 1024 * 1024)
#define BTRFS_MKFS_SMALL_VOLUME_SIZE (1024 * 1024 * 1024)

#define BTRFS_SCAN_MOUNTED	(1ULL << 0)
#define BTRFS_SCAN_LBLKID	(1ULL << 1)

#define BTRFS_UPDATE_KERNEL	1

#define BTRFS_ARG_UNKNOWN	0
#define BTRFS_ARG_MNTPOINT	1
#define BTRFS_ARG_UUID		2
#define BTRFS_ARG_BLKDEV	3
#define BTRFS_ARG_REG		4

#define BTRFS_UUID_UNPARSED_SIZE	37

#define ARGV0_BUF_SIZE	PATH_MAX

#define GETOPT_VAL_SI				256
#define GETOPT_VAL_IEC				257
#define GETOPT_VAL_RAW				258
#define GETOPT_VAL_HUMAN_READABLE		259
#define GETOPT_VAL_KBYTES			260
#define GETOPT_VAL_MBYTES			261
#define GETOPT_VAL_GBYTES			262
#define GETOPT_VAL_TBYTES			263

int check_argc_exact(int nargs, int expected);
int check_argc_min(int nargs, int expected);
int check_argc_max(int nargs, int expected);

void fixup_argv0(char **argv, const char *token);
void set_argv0(char **argv);

/*
 * Output modes of size
 */
#define UNITS_RESERVED			(0)
#define UNITS_BYTES			(1)
#define UNITS_KBYTES			(2)
#define UNITS_MBYTES			(3)
#define UNITS_GBYTES			(4)
#define UNITS_TBYTES			(5)
#define UNITS_RAW			(1U << UNITS_MODE_SHIFT)
#define UNITS_BINARY			(2U << UNITS_MODE_SHIFT)
#define UNITS_DECIMAL			(3U << UNITS_MODE_SHIFT)
#define UNITS_MODE_MASK			((1U << UNITS_MODE_SHIFT) - 1)
#define UNITS_MODE_SHIFT		(8)
#define UNITS_HUMAN_BINARY		(UNITS_BINARY)
#define UNITS_HUMAN_DECIMAL		(UNITS_DECIMAL)
#define UNITS_HUMAN			(UNITS_HUMAN_BINARY)
#define UNITS_DEFAULT			(UNITS_HUMAN)

void units_set_mode(unsigned *units, unsigned mode);
void units_set_base(unsigned *units, unsigned base);

int make_btrfs(int fd, const char *device, const char *label,
	       char *fs_uuid, u64 blocks[6], u64 num_bytes, u32 nodesize,
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
int btrfs_register_one_device(const char *fname);
int btrfs_register_all_devices(void);
char *canonicalize_dm_name(const char *ptname);
char *canonicalize_path(const char *path);
int check_mounted(const char *devicename);
int check_mounted_where(int fd, const char *file, char *where, int size,
			struct btrfs_fs_devices **fs_devices_mnt);
int btrfs_device_already_in_root(struct btrfs_root *root, int fd,
				 int super_offset);

int pretty_size_snprintf(u64 size, char *str, size_t str_bytes, unsigned unit_mode);
#define pretty_size(size) 	pretty_size_mode(size, UNITS_DEFAULT)
#define pretty_size_mode(size, mode) 					      \
	({								      \
		static __thread char _str[32];				      \
		(void)pretty_size_snprintf((size), _str, sizeof(_str), (mode)); \
		_str;							      \
	})

int get_mountpt(char *dev, char *mntpt, size_t size);
int btrfs_scan_block_devices(int run_ioctl);
u64 parse_size(char *s);
u64 arg_strtou64(const char *str);
int open_file_or_dir(const char *fname, DIR **dirstream);
int open_file_or_dir3(const char *fname, DIR **dirstream, int open_flags);
void close_file_or_dir(int fd, DIR *dirstream);
int get_fs_info(char *path, struct btrfs_ioctl_fs_info_args *fi_args,
		struct btrfs_ioctl_dev_info_args **di_ret);
int get_label(const char *btrfs_dev, char *label);
int set_label(const char *btrfs_dev, const char *label);

char *__strncpy__null(char *dest, const char *src, size_t n);
int is_block_device(const char *file);
int is_mount_point(const char *file);
int check_arg_type(const char *input);
int open_path_or_dev_mnt(const char *path, DIR **dirstream);
u64 btrfs_device_size(int fd, struct stat *st);
/* Helper to always get proper size of the destination string */
#define strncpy_null(dest, src) __strncpy__null(dest, src, sizeof(dest))
int test_dev_for_mkfs(char *file, int force_overwrite, char *estr);
int get_label_mounted(const char *mount_path, char *labelp);
int test_num_disk_vs_raid(u64 metadata_profile, u64 data_profile,
	u64 dev_cnt, int mixed, char *estr);
int is_vol_small(char *file);
int csum_tree_block(struct btrfs_root *root, struct extent_buffer *buf,
			   int verify);
int ask_user(char *question);
int lookup_ino_rootid(int fd, u64 *rootid);
int btrfs_scan_lblkid(void);
int get_btrfs_mount(const char *dev, char *mp, size_t mp_size);
int find_mount_root(const char *path, char **mount_root);
int get_device_info(int fd, u64 devid,
		struct btrfs_ioctl_dev_info_args *di_args);
int test_uuid_unique(char *fs_uuid);
u64 disk_size(char *path);
int get_device_info(int fd, u64 devid,
		struct btrfs_ioctl_dev_info_args *di_args);
u64 get_partition_size(char *dev);
const char* group_type_str(u64 flags);
const char* group_profile_str(u64 flags);

int test_minimum_size(const char *file, u32 leafsize);
int test_issubvolname(const char *name);
int test_isdir(const char *path);

/*
 * Btrfs minimum size calculation is complicated, it should include at least:
 * 1. system group size
 * 2. minimum global block reserve
 * 3. metadata used at mkfs
 * 4. space reservation to create uuid for first mount.
 * Also, raid factor should also be taken into consideration.
 * To avoid the overkill calculation, (system group + global block rsv) * 2
 * for *EACH* device should be good enough.
 */
static inline u64 btrfs_min_global_blk_rsv_size(u32 leafsize)
{
	return leafsize << 10;
}
static inline u64 btrfs_min_dev_size(u32 leafsize)
{
	return 2 * (BTRFS_MKFS_SYSTEM_GROUP_SIZE +
		    btrfs_min_global_blk_rsv_size(leafsize));
}

int find_next_key(struct btrfs_path *path, struct btrfs_key *key);
char* btrfs_group_type_str(u64 flag);
char* btrfs_group_profile_str(u64 flag);

/*
 * Get the length of the string converted from a u64 number.
 *
 * Result is equal to log10(num) + 1, but without the use of math library.
 */
static inline int count_digits(u64 num)
{
	int ret = 0;

	if (num == 0)
		return 1;
	while (num > 0) {
		ret++;
		num /= 10;
	}
	return ret;
}

static inline u64 div_factor(u64 num, int factor)
{
	if (factor == 10)
		return num;
	num *= factor;
	num /= 10;
	return num;
}

int btrfs_tree_search2_ioctl_supported(int fd);

#endif
