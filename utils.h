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
#include "internal.h"
#include "btrfs-list.h"

#define BTRFS_MKFS_SYSTEM_GROUP_SIZE (4 * 1024 * 1024)
#define BTRFS_MKFS_SMALL_VOLUME_SIZE (1024 * 1024 * 1024)
#define BTRFS_MKFS_DEFAULT_NODE_SIZE 16384
#define BTRFS_MKFS_DEFAULT_FEATURES 				\
		(BTRFS_FEATURE_INCOMPAT_EXTENDED_IREF		\
		| BTRFS_FEATURE_INCOMPAT_SKINNY_METADATA)

/*
 * Avoid multi-device features (RAID56) and mixed block groups
 */
#define BTRFS_CONVERT_ALLOWED_FEATURES				\
	(BTRFS_FEATURE_INCOMPAT_MIXED_BACKREF			\
	| BTRFS_FEATURE_INCOMPAT_DEFAULT_SUBVOL			\
	| BTRFS_FEATURE_INCOMPAT_COMPRESS_LZO			\
	| BTRFS_FEATURE_INCOMPAT_COMPRESS_LZOv2			\
	| BTRFS_FEATURE_INCOMPAT_BIG_METADATA			\
	| BTRFS_FEATURE_INCOMPAT_EXTENDED_IREF			\
	| BTRFS_FEATURE_INCOMPAT_SKINNY_METADATA		\
	| BTRFS_FEATURE_INCOMPAT_NO_HOLES)

#define BTRFS_CONVERT_META_GROUP_SIZE (32 * 1024 * 1024)

#define BTRFS_FEATURE_LIST_ALL		(1ULL << 63)

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

#define GETOPT_VAL_HELP				270

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

void btrfs_list_all_fs_features(u64 mask_disallowed);
char* btrfs_parse_fs_features(char *namelist, u64 *flags);
void btrfs_process_fs_features(u64 flags);
void btrfs_parse_features_to_string(char *buf, u64 flags);
void print_kernel_version(FILE *stream, u32 version);
u32 get_running_kernel_version(void);

struct btrfs_mkfs_config {
	char *label;
	char fs_uuid[BTRFS_UUID_UNPARSED_SIZE];
	char chunk_uuid[BTRFS_UUID_UNPARSED_SIZE];
	u64 blocks[8];
	u64 num_bytes;
	u32 nodesize;
	u32 sectorsize;
	u32 stripesize;
	u64 features;

	/* Super bytenr after make_btrfs */
	u64 super_bytenr;
};

struct btrfs_convert_context {
	u32 blocksize;
	u32 first_data_block;
	u32 block_count;
	u32 inodes_count;
	u32 free_inodes_count;
	u64 total_bytes;
	char *volume_name;
	const struct btrfs_convert_operations *convert_ops;

	/* The accurate used space of old filesystem */
	struct cache_tree used;

	/* Batched ranges which must be covered by data chunks */
	struct cache_tree data_chunks;

	/* Free space which is not covered by data_chunks */
	struct cache_tree free;

	void *fs_data;
};

#define	PREP_DEVICE_ZERO_END	(1U << 0)
#define	PREP_DEVICE_DISCARD	(1U << 1)
#define	PREP_DEVICE_VERBOSE	(1U << 2)

int make_btrfs(int fd, struct btrfs_mkfs_config *cfg,
		struct btrfs_convert_context *cctx);
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
int get_label(const char *btrfs_dev, char *label);
int set_label(const char *btrfs_dev, const char *label);

char *__strncpy_null(char *dest, const char *src, size_t n);
int is_block_device(const char *file);
int is_mount_point(const char *file);
int check_arg_type(const char *input);
int open_path_or_dev_mnt(const char *path, DIR **dirstream, int verbose);
int btrfs_open_dir(const char *path, DIR **dirstream, int verbose);
u64 btrfs_device_size(int fd, struct stat *st);
/* Helper to always get proper size of the destination string */
#define strncpy_null(dest, src) __strncpy_null(dest, src, sizeof(dest))
int test_dev_for_mkfs(const char *file, int force_overwrite);
int get_label_mounted(const char *mount_path, char *labelp);
int get_label_unmounted(const char *dev, char *label);
int test_num_disk_vs_raid(u64 metadata_profile, u64 data_profile,
	u64 dev_cnt, int mixed, int ssd);
int group_profile_max_safe_loss(u64 flags);
int is_vol_small(const char *file);
int csum_tree_block(struct btrfs_root *root, struct extent_buffer *buf,
			   int verify);
int ask_user(const char *question);
int lookup_ino_rootid(int fd, u64 *rootid);
int btrfs_scan_lblkid(void);
int get_btrfs_mount(const char *dev, char *mp, size_t mp_size);
int find_mount_root(const char *path, char **mount_root);
int get_device_info(int fd, u64 devid,
		struct btrfs_ioctl_dev_info_args *di_args);
int test_uuid_unique(char *fs_uuid);
u64 disk_size(const char *path);
u64 get_partition_size(const char *dev);

int test_minimum_size(const char *file, u32 nodesize);
int test_issubvolname(const char *name);
int test_issubvolume(const char *path);
int test_isdir(const char *path);

const char *subvol_strip_mountpoint(const char *mnt, const char *full_path);
int get_subvol_info(const char *fullpath, struct root_info *get_ri);

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
static inline u64 btrfs_min_global_blk_rsv_size(u32 nodesize)
{
	return (u64)nodesize << 10;
}

static inline u64 btrfs_min_dev_size(u32 nodesize)
{
	return 2 * (BTRFS_MKFS_SYSTEM_GROUP_SIZE +
		    btrfs_min_global_blk_rsv_size(nodesize));
}

int find_next_key(struct btrfs_path *path, struct btrfs_key *key);
const char* btrfs_group_type_str(u64 flag);
const char* btrfs_group_profile_str(u64 flag);

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
int btrfs_check_nodesize(u32 nodesize, u32 sectorsize, u64 features);

const char *get_argv0_buf(void);

#define HELPINFO_UNITS_LONG							\
	"--raw              raw numbers in bytes",				\
	"--human-readable   human friendly numbers, base 1024 (default)",	\
	"--iec              use 1024 as a base (KiB, MiB, GiB, TiB)",		\
	"--si               use 1000 as a base (kB, MB, GB, TB)",		\
	"--kbytes           show sizes in KiB, or kB with --si",		\
	"--mbytes           show sizes in MiB, or MB with --si",		\
	"--gbytes           show sizes in GiB, or GB with --si",		\
	"--tbytes           show sizes in TiB, or TB with --si"

#define HELPINFO_UNITS_SHORT_LONG						\
	"-b|--raw           raw numbers in bytes",				\
	"-h|--human-readable",							\
	"                   human friendly numbers, base 1024 (default)",	\
	"-H                 human friendly numbers, base 1000",			\
	"--iec              use 1024 as a base (KiB, MiB, GiB, TiB)",		\
	"--si               use 1000 as a base (kB, MB, GB, TB)",		\
	"-k|--kbytes        show sizes in KiB, or kB with --si",		\
	"-m|--mbytes        show sizes in MiB, or MB with --si",		\
	"-g|--gbytes        show sizes in GiB, or GB with --si",		\
	"-t|--tbytes        show sizes in TiB, or TB with --si"

unsigned int get_unit_mode_from_arg(int *argc, char *argv[], int df_mode);
void clean_args_no_options(int argc, char *argv[], const char * const *usage);
void clean_args_no_options_relaxed(int argc, char *argv[],
		const char * const *usagestr);
int string_is_numerical(const char *str);

#if DEBUG_VERBOSE_ERROR
#define	PRINT_VERBOSE_ERROR	fprintf(stderr, "%s:%d:", __FILE__, __LINE__)
#else
#define PRINT_VERBOSE_ERROR
#endif

#if DEBUG_TRACE_ON_ERROR
#define PRINT_TRACE_ON_ERROR	print_trace()
#else
#define PRINT_TRACE_ON_ERROR
#endif

#if DEBUG_ABORT_ON_ERROR
#define DO_ABORT_ON_ERROR	abort()
#else
#define DO_ABORT_ON_ERROR
#endif

#define error(fmt, ...)							\
	do {								\
		PRINT_TRACE_ON_ERROR;					\
		PRINT_VERBOSE_ERROR;					\
		__error((fmt), ##__VA_ARGS__);				\
		DO_ABORT_ON_ERROR;					\
	} while (0)

#define error_on(cond, fmt, ...)					\
	do {								\
		if ((cond))						\
			PRINT_TRACE_ON_ERROR;				\
		if ((cond))						\
			PRINT_VERBOSE_ERROR;				\
		__error_on((cond), (fmt), ##__VA_ARGS__);		\
		if ((cond))						\
			DO_ABORT_ON_ERROR;				\
	} while (0)

#define warning(fmt, ...)						\
	do {								\
		PRINT_TRACE_ON_ERROR;					\
		PRINT_VERBOSE_ERROR;					\
		__warning((fmt), ##__VA_ARGS__);			\
	} while (0)

#define warning_on(cond, fmt, ...)					\
	do {								\
		if ((cond))						\
			PRINT_TRACE_ON_ERROR;				\
		if ((cond))						\
			PRINT_VERBOSE_ERROR;				\
		__warning_on((cond), (fmt), ##__VA_ARGS__);		\
	} while (0)

__attribute__ ((format (printf, 1, 2)))
static inline void __warning(const char *fmt, ...)
{
	va_list args;

	fputs("WARNING: ", stderr);
	va_start(args, fmt);
	vfprintf(stderr, fmt, args);
	va_end(args);
	fputc('\n', stderr);
}

__attribute__ ((format (printf, 1, 2)))
static inline void __error(const char *fmt, ...)
{
	va_list args;

	fputs("ERROR: ", stderr);
	va_start(args, fmt);
	vfprintf(stderr, fmt, args);
	va_end(args);
	fputc('\n', stderr);
}

__attribute__ ((format (printf, 2, 3)))
static inline int __warning_on(int condition, const char *fmt, ...)
{
	va_list args;

	if (!condition)
		return 0;

	fputs("WARNING: ", stderr);
	va_start(args, fmt);
	vfprintf(stderr, fmt, args);
	va_end(args);
	fputc('\n', stderr);

	return 1;
}

__attribute__ ((format (printf, 2, 3)))
static inline int __error_on(int condition, const char *fmt, ...)
{
	va_list args;

	if (!condition)
		return 0;

	fputs("ERROR: ", stderr);
	va_start(args, fmt);
	vfprintf(stderr, fmt, args);
	va_end(args);
	fputc('\n', stderr);

	return 1;
}

/* Pseudo random number generator wrappers */
u32 rand_u32(void);

static inline int rand_int(void)
{
	return (int)(rand_u32());
}

static inline u64 rand_u64(void)
{
	u64 ret = 0;

	ret += rand_u32();
	ret <<= 32;
	ret += rand_u32();
	return ret;
}

static inline u16 rand_u16(void)
{
	return (u16)(rand_u32());
}

static inline u8 rand_u8(void)
{
	return (u8)(rand_u32());
}

/* Return random number in range [0, limit) */
unsigned int rand_range(unsigned int upper);

/* Also allow setting the seed manually */
void init_rand_seed(u64 seed);

#endif
