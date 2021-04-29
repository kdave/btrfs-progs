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
#include "kernel-shared/ctree.h"
#include <dirent.h>
#include <stdarg.h>
#include "common/defs.h"
#include "common/internal.h"
#include "btrfs-list.h"
#include "kernel-lib/sizes.h"
#include "common/messages.h"
#include "ioctl.h"
#include "common/fsfeatures.h"

enum exclusive_operation {
	BTRFS_EXCLOP_NONE,
	BTRFS_EXCLOP_BALANCE,
	BTRFS_EXCLOP_DEV_ADD,
	BTRFS_EXCLOP_DEV_REMOVE,
	BTRFS_EXCLOP_DEV_REPLACE,
	BTRFS_EXCLOP_RESIZE,
	BTRFS_EXCLOP_SWAP_ACTIVATE,
	BTRFS_EXCLOP_UNKNOWN = -1,
};

enum btrfs_csum_type parse_csum_type(const char *s);
u64 parse_size_from_string(const char *s);
u64 parse_qgroupid(const char *p);
u64 arg_strtou64(const char *str);
int get_fs_info(const char *path, struct btrfs_ioctl_fs_info_args *fi_args,
		struct btrfs_ioctl_dev_info_args **di_ret);
int get_fsid(const char *path, u8 *fsid, int silent);
int get_fsid_fd(int fd, u8 *fsid);
int get_fs_exclop(int fd);
int check_running_fs_exclop(int fd, enum exclusive_operation start, bool enqueue);
const char *get_fs_exclop_name(int op);

int get_label(const char *btrfs_dev, char *label);
int set_label(const char *btrfs_dev, const char *label);

int check_arg_type(const char *input);
int get_label_mounted(const char *mount_path, char *labelp);
int get_label_unmounted(const char *dev, char *label);
int group_profile_max_safe_loss(u64 flags);
int csum_tree_block(struct btrfs_fs_info *root, struct extent_buffer *buf,
		    int verify);
int ask_user(const char *question);
int lookup_path_rootid(int fd, u64 *rootid);
int find_mount_fsroot(const char *subvol, const char *subvolid, char **mount);
int find_mount_root(const char *path, char **mount_root);
int get_device_info(int fd, u64 devid,
		struct btrfs_ioctl_dev_info_args *di_args);
int get_df(int fd, struct btrfs_ioctl_space_args **sargs_ret);
int test_uuid_unique(char *fs_uuid);

const char *subvol_strip_mountpoint(const char *mnt, const char *full_path);
int find_next_key(struct btrfs_path *path, struct btrfs_key *key);
const char* btrfs_group_type_str(u64 flag);
const char* btrfs_group_profile_str(u64 flag);

int count_digits(u64 num);
u64 div_factor(u64 num, int factor);

int btrfs_tree_search2_ioctl_supported(int fd);

int string_is_numerical(const char *str);
int prefixcmp(const char *str, const char *prefix);

unsigned long total_memory(void);

void print_device_info(struct btrfs_device *device, char *prefix);
void print_all_devices(struct list_head *devices);

#define BTRFS_BCONF_UNSET	-1
#define BTRFS_BCONF_QUIET	 0
/*
 * Global program state, configurable by command line and available to
 * functions without extra context passing.
 */
struct btrfs_config {
	unsigned int output_format;

	/*
	 * Values:
	 *   BTRFS_BCONF_QUIET
	 *   BTRFS_BCONF_UNSET
	 *   > 0: verbose level
	 */
	int verbose;
};
extern struct btrfs_config bconf;

void btrfs_config_init(void);
void bconf_be_verbose(void);
void bconf_be_quiet(void);

/* Pseudo random number generator wrappers */
int rand_int(void);
u8 rand_u8(void);
u16 rand_u16(void);
u32 rand_u32(void);
u64 rand_u64(void);
unsigned int rand_range(unsigned int upper);
void init_rand_seed(u64 seed);

char *btrfs_test_for_multiple_profiles(int fd);
int btrfs_warn_multiple_profiles(int fd);

int sysfs_open_file(const char *name);
int sysfs_open_fsid_file(int fd, const char *filename);
int sysfs_read_file(int fd, char *buf, size_t size);

#endif
