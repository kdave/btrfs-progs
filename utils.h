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
#include <stdarg.h>
#include "common-defs.h"
#include "internal.h"
#include "btrfs-list.h"
#include "sizes.h"
#include "messages.h"
#include "ioctl.h"

#define BTRFS_SCAN_MOUNTED	(1ULL << 0)
#define BTRFS_SCAN_LBLKID	(1ULL << 1)

#define BTRFS_UPDATE_KERNEL	1

#define BTRFS_ARG_UNKNOWN	0
#define BTRFS_ARG_MNTPOINT	1
#define BTRFS_ARG_UUID		2
#define BTRFS_ARG_BLKDEV	3
#define BTRFS_ARG_REG		4

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
/* Interpret the u64 value as s64 */
#define UNITS_NEGATIVE			(4U << UNITS_MODE_SHIFT)
#define UNITS_MODE_MASK			((1U << UNITS_MODE_SHIFT) - 1)
#define UNITS_MODE_SHIFT		(8)
#define UNITS_HUMAN_BINARY		(UNITS_BINARY)
#define UNITS_HUMAN_DECIMAL		(UNITS_DECIMAL)
#define UNITS_HUMAN			(UNITS_HUMAN_BINARY)
#define UNITS_DEFAULT			(UNITS_HUMAN)

void units_set_mode(unsigned *units, unsigned mode);
void units_set_base(unsigned *units, unsigned base);

#define	PREP_DEVICE_ZERO_END	(1U << 0)
#define	PREP_DEVICE_DISCARD	(1U << 1)
#define	PREP_DEVICE_VERBOSE	(1U << 2)

#define SEEN_FSID_HASH_SIZE 256
struct seen_fsid {
	u8 fsid[BTRFS_FSID_SIZE];
	struct seen_fsid *next;
	DIR *dirstream;
	int fd;
};

int btrfs_make_root_dir(struct btrfs_trans_handle *trans,
			struct btrfs_root *root, u64 objectid);
int btrfs_prepare_device(int fd, const char *file, u64 *block_count_ret,
		u64 max_block_count, unsigned opflags);
int btrfs_add_to_fsid(struct btrfs_trans_handle *trans,
		      struct btrfs_root *root, int fd, const char *path,
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
const char *pretty_size_mode(u64 size, unsigned mode);

u64 parse_size(char *s);
u64 parse_qgroupid(const char *p);
u64 arg_strtou64(const char *str);
int arg_copy_path(char *dest, const char *src, int destlen);
int open_file_or_dir(const char *fname, DIR **dirstream);
int open_file_or_dir3(const char *fname, DIR **dirstream, int open_flags);
void close_file_or_dir(int fd, DIR *dirstream);
int get_fs_info(const char *path, struct btrfs_ioctl_fs_info_args *fi_args,
		struct btrfs_ioctl_dev_info_args **di_ret);
int get_fsid(const char *path, u8 *fsid, int silent);

int is_seen_fsid(u8 *fsid, struct seen_fsid *seen_fsid_hash[]);
int add_seen_fsid(u8 *fsid, struct seen_fsid *seen_fsid_hash[],
		int fd, DIR *dirstream);
void free_seen_fsid(struct seen_fsid *seen_fsid_hash[]);

int get_label(const char *btrfs_dev, char *label);
int set_label(const char *btrfs_dev, const char *label);

char *__strncpy_null(char *dest, const char *src, size_t n);
int is_block_device(const char *file);
int is_mount_point(const char *file);
int is_path_exist(const char *file);
int is_reg_file(const char *path);
int check_arg_type(const char *input);
int open_path_or_dev_mnt(const char *path, DIR **dirstream, int verbose);
int btrfs_open(const char *path, DIR **dirstream, int verbose, int dir_only);
int btrfs_open_dir(const char *path, DIR **dirstream, int verbose);
int btrfs_open_file_or_dir(const char *path, DIR **dirstream, int verbose);
u64 btrfs_device_size(int fd, struct stat *st);
/* Helper to always get proper size of the destination string */
#define strncpy_null(dest, src) __strncpy_null(dest, src, sizeof(dest))
int get_label_mounted(const char *mount_path, char *labelp);
int get_label_unmounted(const char *dev, char *label);
int group_profile_max_safe_loss(u64 flags);
int csum_tree_block(struct btrfs_fs_info *root, struct extent_buffer *buf,
		    int verify);
int ask_user(const char *question);
int lookup_path_rootid(int fd, u64 *rootid);
int btrfs_scan_devices(void);
int get_btrfs_mount(const char *dev, char *mp, size_t mp_size);
int find_mount_root(const char *path, char **mount_root);
int get_device_info(int fd, u64 devid,
		struct btrfs_ioctl_dev_info_args *di_args);
int test_uuid_unique(char *fs_uuid);
u64 disk_size(const char *path);
u64 get_partition_size(const char *dev);

int test_issubvolname(const char *name);
int test_issubvolume(const char *path);
int test_isdir(const char *path);

const char *subvol_strip_mountpoint(const char *mnt, const char *full_path);
int get_subvol_info(const char *fullpath, struct root_info *get_ri);
int get_subvol_info_by_rootid(const char *mnt, struct root_info *get_ri,
							u64 rootid_arg);
int get_subvol_info_by_uuid(const char *mnt, struct root_info *get_ri,
							u8 *uuid_arg);
int find_next_key(struct btrfs_path *path, struct btrfs_key *key);
const char* btrfs_group_type_str(u64 flag);
const char* btrfs_group_profile_str(u64 flag);

int count_digits(u64 num);
u64 div_factor(u64 num, int factor);

int btrfs_tree_search2_ioctl_supported(int fd);

unsigned int get_unit_mode_from_arg(int *argc, char *argv[], int df_mode);
int string_is_numerical(const char *str);
int prefixcmp(const char *str, const char *prefix);

unsigned long total_memory(void);

/*
 * Global program state, configurable by command line and available to
 * functions without extra context passing.
 */
struct btrfs_config {
};
extern struct btrfs_config bconf;

void btrfs_config_init(void);

/* Pseudo random number generator wrappers */
int rand_int(void);
u8 rand_u8(void);
u16 rand_u16(void);
u32 rand_u32(void);
u64 rand_u64(void);
unsigned int rand_range(unsigned int upper);
void init_rand_seed(u64 seed);

#endif
