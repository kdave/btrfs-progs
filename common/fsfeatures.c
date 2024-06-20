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

#include "kerncompat.h"
#include <sys/utsname.h>
#include <sys/ioctl.h>
#include <linux/version.h>
#include <unistd.h>
#include <errno.h>
#include <stdbool.h>
#include <stdlib.h>
#include <string.h>
#include "kernel-lib/sizes.h"
#include "kernel-shared/uapi/btrfs.h"
#include "kernel-shared/uapi/btrfs_tree.h"
#include "kernel-shared/ctree.h"
#include "common/fsfeatures.h"
#include "common/string-utils.h"
#include "common/sysfs-utils.h"
#include "common/messages.h"
#include "common/tree-search.h"

/*
 * Insert a root item for temporary tree root
 *
 * Only used in make_btrfs_v2().
 */
#define VERSION_TO_STRING3(name, a,b,c)				\
	.name ## _str = #a "." #b "." #c,			\
	.name ## _ver = KERNEL_VERSION(a,b,c)
#define VERSION_TO_STRING2(name, a,b)				\
	.name ## _str = #a "." #b,				\
	.name ## _ver = KERNEL_VERSION(a,b,0)
#define VERSION_NULL(name)					\
	.name ## _str = NULL,					\
	.name ## _ver = 0

/*
 * For feature names that are only an alias we don't need to duplicate
 * versions.
 *
 * When compat_str is NULL, the feature descriptor is an alias.
 */
#define VERSION_ALIAS						\
		VERSION_NULL(compat),				\
		VERSION_NULL(safe),				\
		VERSION_NULL(default)

enum feature_source {
	FS_FEATURES,
	RUNTIME_FEATURES,
};

/*
 * Feature stability status and versions: compat <= safe <= default
 */
struct btrfs_feature {
	const char *name;

	/*
	 * At least one of the bit must be set in the following *_flag member.
	 *
	 * For features like list-all and quota which don't have any
	 * incompat/compat_ro bit set, it go to runtime_flag.
	 */
	u64 incompat_flag;
	u64 compat_ro_flag;
	u64 runtime_flag;

	const char *sysfs_name;
	/*
	 * Compatibility with kernel of given version. Filesystem can be
	 * mounted.
	 */
	const char *compat_str;
	u32 compat_ver;
	/*
	 * Considered safe for use, but is not on by default, even if the
	 * kernel supports the feature.
	 */
	const char *safe_str;
	u32 safe_ver;
	/*
	 * Considered safe for use and will be turned on by default if
	 * supported by the running kernel.
	 */
	const char *default_str;
	u32 default_ver;
	const char *desc;
};

/*
 * Keep the list sorted by compat version.
 */
static const struct btrfs_feature mkfs_features[] = {
	{
		.name		= "mixed-bg",
		.incompat_flag	= BTRFS_FEATURE_INCOMPAT_MIXED_GROUPS,
		.sysfs_name	= "mixed_groups",
		VERSION_TO_STRING3(compat, 2,6,37),
		VERSION_TO_STRING3(safe, 2,6,37),
		VERSION_NULL(default),
		.desc		= "mixed data and metadata block groups"
	},
	{
		.name		= "quota",
		.runtime_flag	= BTRFS_FEATURE_RUNTIME_QUOTA,
		.sysfs_name	= NULL,
		VERSION_TO_STRING2(compat, 3,4),
		VERSION_NULL(safe),
		VERSION_NULL(default),
		.desc		= "hierarchical quota group support (qgroups)"
	},
	{
		.name		= "extref",
		.incompat_flag	= BTRFS_FEATURE_INCOMPAT_EXTENDED_IREF,
		.sysfs_name	= "extended_iref",
		VERSION_TO_STRING2(compat, 3,7),
		VERSION_TO_STRING2(safe, 3,12),
		VERSION_TO_STRING2(default, 3,12),
		.desc		= "increased hardlink limit per file to 65536"
	}, {
		.name		= "raid56",
		.incompat_flag	= BTRFS_FEATURE_INCOMPAT_RAID56,
		.sysfs_name	= "raid56",
		VERSION_TO_STRING2(compat, 3,9),
		VERSION_NULL(safe),
		VERSION_NULL(default),
		.desc		= "raid56 extended format"
	}, {
		.name		= "skinny-metadata",
		.incompat_flag	= BTRFS_FEATURE_INCOMPAT_SKINNY_METADATA,
		.sysfs_name	= "skinny_metadata",
		VERSION_TO_STRING2(compat, 3,10),
		VERSION_TO_STRING2(safe, 3,18),
		VERSION_TO_STRING2(default, 3,18),
		.desc		= "reduced-size metadata extent refs"
	}, {
		.name		= "no-holes",
		.incompat_flag	= BTRFS_FEATURE_INCOMPAT_NO_HOLES,
		.sysfs_name	= "no_holes",
		VERSION_TO_STRING2(compat, 3,14),
		VERSION_TO_STRING2(safe, 4,0),
		VERSION_TO_STRING2(default, 5,15),
		.desc		= "no explicit hole extents for files"
	},
	{
		.name		= "fst",
		.compat_ro_flag	= BTRFS_FEATURE_COMPAT_RO_FREE_SPACE_TREE |
				  BTRFS_FEATURE_COMPAT_RO_FREE_SPACE_TREE_VALID,
		.sysfs_name = "free_space_tree",
		VERSION_ALIAS,
		.desc		= "free-space-tree alias"
	},
	{
		.name		= "free-space-tree",
		.compat_ro_flag	= BTRFS_FEATURE_COMPAT_RO_FREE_SPACE_TREE |
				  BTRFS_FEATURE_COMPAT_RO_FREE_SPACE_TREE_VALID,
		.sysfs_name = "free_space_tree",
		VERSION_TO_STRING2(compat, 4,5),
		VERSION_TO_STRING2(safe, 4,9),
		VERSION_TO_STRING2(default, 5,15),
		.desc		= "free space tree, improved space tracking (space_cache=v2)"
	},
	{
		.name		= "raid1c34",
		.incompat_flag	= BTRFS_FEATURE_INCOMPAT_RAID1C34,
		.sysfs_name	= "raid1c34",
		VERSION_TO_STRING2(compat, 5,5),
		VERSION_NULL(safe),
		VERSION_NULL(default),
		.desc		= "RAID1 with 3 or 4 copies"
	},
#ifdef BTRFS_ZONED
	{
		.name		= "zoned",
		.incompat_flag	= BTRFS_FEATURE_INCOMPAT_ZONED,
		.sysfs_name	= "zoned",
		VERSION_TO_STRING2(compat, 5,12),
		VERSION_NULL(safe),
		VERSION_NULL(default),
		.desc		= "support zoned (SMR/ZBC/ZNS) devices"
	},
#endif
#if EXPERIMENTAL
	{
		.name		= "extent-tree-v2",
		.incompat_flag	= BTRFS_FEATURE_INCOMPAT_EXTENT_TREE_V2,
		.sysfs_name	= "extent_tree_v2",
		VERSION_TO_STRING2(compat, 5,15),
		VERSION_NULL(safe),
		VERSION_NULL(default),
		.desc		= "new extent tree format"
	},
#endif
	{
		.name		= "bgt",
		.compat_ro_flag	= BTRFS_FEATURE_COMPAT_RO_BLOCK_GROUP_TREE,
		.sysfs_name	= "block_group_tree",
		VERSION_ALIAS,
		.desc		= "block-group-tree alias"
	},
	{
		.name		= "block-group-tree",
		.compat_ro_flag	= BTRFS_FEATURE_COMPAT_RO_BLOCK_GROUP_TREE,
		.sysfs_name	= "block_group_tree",
		VERSION_TO_STRING2(compat, 6,1),
		VERSION_NULL(safe),
		VERSION_NULL(default),
		.desc		= "block group tree, more efficient block group tracking to reduce mount time"
	},
#if EXPERIMENTAL
	{
		.name		= "rst",
		.incompat_flag	= BTRFS_FEATURE_INCOMPAT_RAID_STRIPE_TREE,
		.sysfs_name	= "raid_stripe_tree",
		VERSION_ALIAS,
		.desc		= "raid-stripe-tree alias"
	},
	{
		.name		= "raid-stripe-tree",
		.incompat_flag	= BTRFS_FEATURE_INCOMPAT_RAID_STRIPE_TREE,
		.sysfs_name	= "raid_stripe_tree",
		VERSION_TO_STRING2(compat, 6,7),
		VERSION_NULL(safe),
		VERSION_NULL(default),
		.desc		= "raid stripe tree, enhanced file extent tracking"
	},
#endif
	{
		.name		= "squota",
		.incompat_flag	= BTRFS_FEATURE_INCOMPAT_SIMPLE_QUOTA,
		.sysfs_name	= "simple_quota",
		VERSION_TO_STRING2(compat, 6,7),
		VERSION_NULL(safe),
		VERSION_NULL(default),
		.desc		= "squota support (simple accounting qgroups)"
	},
	/* Keep this one last */
	{
		.name		= "list-all",
		.runtime_flag	= BTRFS_FEATURE_RUNTIME_LIST_ALL,
		.sysfs_name	= NULL,
		VERSION_NULL(compat),
		VERSION_NULL(safe),
		VERSION_NULL(default),
		.desc		= NULL
	}
};

static const struct btrfs_feature runtime_features[] = {
	{
		.name		= "quota",
		.runtime_flag	= BTRFS_FEATURE_RUNTIME_QUOTA,
		.sysfs_name	= NULL,
		VERSION_TO_STRING2(compat, 3,4),
		VERSION_NULL(safe),
		VERSION_NULL(default),
		.desc		= "quota support (qgroups)"
	}, {
		.name		= "free-space-tree",
		.compat_ro_flag	= BTRFS_FEATURE_COMPAT_RO_FREE_SPACE_TREE |
				  BTRFS_FEATURE_COMPAT_RO_FREE_SPACE_TREE_VALID,
		.sysfs_name	= "free_space_tree",
		VERSION_TO_STRING2(compat, 4,5),
		VERSION_TO_STRING2(safe, 4,9),
		VERSION_TO_STRING2(default, 5,15),
		.desc		= "free space tree (space_cache=v2)"
	},
	/* Keep this one last */
	{
		.name		= "list-all",
		.runtime_flag	= BTRFS_FEATURE_RUNTIME_LIST_ALL,
		.sysfs_name	= NULL,
		VERSION_NULL(compat),
		VERSION_NULL(safe),
		VERSION_NULL(default),
		.desc		= NULL
	}
};

static bool feature_name_is_alias(const struct btrfs_feature *feature)
{
	return feature->compat_str == NULL;
}

/*
 * This is a sanity check to make sure BTRFS_FEATURE_STRING_BUF_SIZE is large
 * enough to contain all strings.
 *
 * All callers using btrfs_parse_*_features_to_string() should call this first.
 */
void btrfs_assert_feature_buf_size(void)
{
	int total_size = 0;
	int i;

	/*
	 * This is a little over-calculated, as we include ", list-all".
	 * But 10 extra bytes should not be a big deal.
	 */
	for (i = 0; i < ARRAY_SIZE(mkfs_features); i++)
		/* The extra 2 bytes are for the ", " prefix. */
		total_size += strlen(mkfs_features[i].name) + 2;

	if (BTRFS_FEATURE_STRING_BUF_SIZE < total_size) {
		internal_error("string buffer for freature list too small: want %d\n",
			       total_size);
		abort();
	}

	total_size = 0;
	for (i = 0; i < ARRAY_SIZE(runtime_features); i++)
		total_size += strlen(runtime_features[i].name) + 2;
	if (BTRFS_FEATURE_STRING_BUF_SIZE < total_size) {
		internal_error("string buffer for freature list too small: want %d\n",
			       total_size);
		abort();
	}
}

static size_t get_feature_array_size(enum feature_source source)
{
	if (source == FS_FEATURES)
		return ARRAY_SIZE(mkfs_features);
	if (source == RUNTIME_FEATURES)
		return ARRAY_SIZE(runtime_features);
	return 0;
}

static const struct btrfs_feature *get_feature(int i, enum feature_source source)
{
	if (source == FS_FEATURES)
		return &mkfs_features[i];
	if (source == RUNTIME_FEATURES)
		return &runtime_features[i];
	return NULL;
}

static int parse_one_fs_feature(const char *name,
				struct btrfs_mkfs_features *features,
				enum feature_source source)
{
	const int array_size = get_feature_array_size(source);
	int i;
	int found = 0;

	for (i = 0; i < array_size; i++) {
		const struct btrfs_feature *feat = get_feature(i, source);

		if (name[0] == '^' && !strcmp(feat->name, name + 1)) {
			features->compat_ro_flags &= ~feat->compat_ro_flag;
			features->incompat_flags &= ~feat->incompat_flag;
			features->runtime_flags &= ~feat->runtime_flag;
			found = 1;
		} else if (!strcmp(feat->name, name)) {
			features->compat_ro_flags |= feat->compat_ro_flag;
			features->incompat_flags |= feat->incompat_flag;
			features->runtime_flags |= feat->runtime_flag;
			found = 1;
		}
	}

	return !found;
}

static void parse_features_to_string(char *buf,
				     const struct btrfs_mkfs_features *features,
				     enum feature_source source)
{
	const int array_size = get_feature_array_size(source);
	int i;

	buf[0] = 0;

	for (i = 0; i < array_size; i++) {
		const struct btrfs_feature *feat = get_feature(i, source);

		if (feature_name_is_alias(feat))
			continue;

		if (features->compat_ro_flags & feat->compat_ro_flag ||
		    features->incompat_flags & feat->incompat_flag ||
		    features->runtime_flags & feat->runtime_flag) {
			if (*buf)
				strcat(buf, ", ");
			strcat(buf, feat->name);
		}
	}
}

void btrfs_parse_fs_features_to_string(char *buf,
		const struct btrfs_mkfs_features *features)
{
	parse_features_to_string(buf, features, FS_FEATURES);
}

void btrfs_parse_runtime_features_to_string(char *buf,
		const struct btrfs_mkfs_features *features)
{
	parse_features_to_string(buf, features, RUNTIME_FEATURES);
}

static void process_features(struct btrfs_mkfs_features *features,
			     enum feature_source source)
{
	const int array_size = get_feature_array_size(source);
	int i;

	for (i = 0; i < array_size; i++) {
		const struct btrfs_feature *feat = get_feature(i, source);

		if ((features->compat_ro_flags & feat->compat_ro_flag ||
		     features->incompat_flags & feat->incompat_flag ||
		     features->runtime_flags & feat->runtime_flag) &&
		    feat->name && feat->desc) {
			printf("Turning ON incompat feature '%s': %s\n",
				feat->name, feat->desc);
		}
	}
}

void btrfs_process_fs_features(struct btrfs_mkfs_features *features)
{
	process_features(features, FS_FEATURES);
}

void btrfs_process_runtime_features(struct btrfs_mkfs_features *features)
{
	process_features(features, RUNTIME_FEATURES);
}

static void list_all_features(const struct btrfs_mkfs_features *allowed,
			      enum feature_source source)
{
	const int array_size = get_feature_array_size(source);
	int i;
	char *prefix;

	if (source == FS_FEATURES)
		prefix = "Filesystem";
	else if (source == RUNTIME_FEATURES)
		prefix = "Runtime";
	else
		prefix = "UNKNOWN";

	fprintf(stderr, "%s features available:\n", prefix);
	for (i = 0; i < array_size - 1; i++) {
		const struct btrfs_feature *feat = get_feature(i, source);
		const char *sep = "";

		/* The feature is not in the allowed one, skip it. */
		if (allowed &&
		    !(feat->compat_ro_flag & allowed->compat_ro_flags ||
		      feat->incompat_flag & allowed->incompat_flags ||
		      feat->runtime_flag & allowed->runtime_flags))
			continue;

		fprintf(stderr, "%-20s- %s", feat->name, feat->desc);
		if (feature_name_is_alias(feat)) {
			fprintf(stderr, "\n");
			continue;
		}
		fprintf(stderr, " (");
		if (feat->compat_ver) {
			fprintf(stderr, "compat=%s", feat->compat_str);
			sep = ", ";
		}
		if (feat->safe_ver) {
			fprintf(stderr, "%ssafe=%s", sep, feat->safe_str);
			sep = ", ";
		}
		if (feat->default_ver)
			fprintf(stderr, "%sdefault=%s", sep, feat->default_str);
		fprintf(stderr, ")\n");
	}
}

/* @allowed can be null, then all features will be listed. */
void btrfs_list_all_fs_features(const struct btrfs_mkfs_features *allowed)
{
	list_all_features(allowed, FS_FEATURES);
}

/* @allowed can be null, then all runtime features will be listed. */
void btrfs_list_all_runtime_features(const struct btrfs_mkfs_features *allowed)
{
	list_all_features(allowed, RUNTIME_FEATURES);
}

/*
 * Return NULL if all features were parsed fine, otherwise return the name of
 * the first unparsed.
 */
static char *parse_features(char *namelist,
			    struct btrfs_mkfs_features *features,
			    enum feature_source source)
{
	char *this_char;
	char *save_ptr = NULL; /* Satisfy static checkers */

	for (this_char = strtok_r(namelist, ",", &save_ptr);
	     this_char != NULL;
	     this_char = strtok_r(NULL, ",", &save_ptr)) {
		if (parse_one_fs_feature(this_char, features, source))
			return this_char;
	}

	return NULL;
}

char *btrfs_parse_fs_features(char *namelist,
		struct btrfs_mkfs_features *features)
{
	return parse_features(namelist, features, FS_FEATURES);
}

char *btrfs_parse_runtime_features(char *namelist,
		struct btrfs_mkfs_features *features)
{
	return parse_features(namelist, features, RUNTIME_FEATURES);
}

void print_kernel_version(FILE *stream, u32 version)
{
	u32 v[3];

	v[0] = version & 0xFF;
	v[1] = (version >> 8) & 0xFF;
	v[2] = version >> 16;
	fprintf(stream, "%u.%u", v[2], v[1]);
	if (v[0])
		fprintf(stream, ".%u", v[0]);
}

u32 get_running_kernel_version(void)
{
	struct utsname utsbuf;
	char *tmp;
	char *saveptr = NULL;
	u32 version;

	uname(&utsbuf);
	if (strcmp(utsbuf.sysname, "Linux") != 0) {
		error("unsupported system: %s", utsbuf.sysname);
		exit(1);
	}
	/* 1.2.3-4-name */
	tmp = strchr(utsbuf.release, '-');
	if (tmp)
		*tmp = 0;

	tmp = strtok_r(utsbuf.release, ".", &saveptr);
	if (!string_is_numerical(tmp))
		return (u32)-1;
	version = atoi(tmp) << 16;
	tmp = strtok_r(NULL, ".", &saveptr);
	if (!string_is_numerical(tmp))
		return (u32)-1;
	version |= atoi(tmp) << 8;
	tmp = strtok_r(NULL, ".", &saveptr);
	/* Relaxed format accepts eg. 1.2.3+ */
	if (tmp && string_is_numerical(tmp))
		version |= atoi(tmp);

	return version;
}

/*
 * The buffer size is strlen of "4096 8192 16384 32768 65536", which is 28,
 * then round up to 32.
 */
#define SUPPORTED_SECTORSIZE_BUF_SIZE	32

/*
 * Check if current kernel supports the given size
 */
static bool check_supported_sectorsize(u32 sectorsize)
{
	char supported_buf[SUPPORTED_SECTORSIZE_BUF_SIZE] = { 0 };
	char sectorsize_buf[SUPPORTED_SECTORSIZE_BUF_SIZE] = { 0 };
	char *this_char;
	char *save_ptr = NULL;
	int fd;
	int ret;

	fd = sysfs_open_file("features/supported_sectorsizes");
	if (fd < 0)
		return false;
	ret = sysfs_read_file(fd, supported_buf, SUPPORTED_SECTORSIZE_BUF_SIZE);
	close(fd);
	if (ret < 0)
		return false;
	snprintf(sectorsize_buf, SUPPORTED_SECTORSIZE_BUF_SIZE, "%u", sectorsize);

	for (this_char = strtok_r(supported_buf, " ", &save_ptr);
	     this_char != NULL;
	     this_char = strtok_r(NULL, " ", &save_ptr)) {
		/*
		 * Also check the terminal '\0' to handle cases like
		 * "4096" and "40960".
		 */
		if (!strncmp(this_char, sectorsize_buf, strlen(sectorsize_buf) + 1))
			return true;
	}
	return false;
}

int btrfs_check_sectorsize(u32 sectorsize)
{
	bool sectorsize_checked = false;
	u32 page_size = (u32)sysconf(_SC_PAGESIZE);

	if (!is_power_of_2(sectorsize)) {
		error("invalid sectorsize %u, must be power of 2", sectorsize);
		return -EINVAL;
	}
	if (sectorsize < SZ_4K || sectorsize > SZ_64K) {
		error("invalid sectorsize %u, expected range is [4K, 64K]",
		      sectorsize);
		return -EINVAL;
	}
	if (page_size == sectorsize)
		sectorsize_checked = true;
	else
		sectorsize_checked = check_supported_sectorsize(sectorsize);

	if (!sectorsize_checked)
		warning(
"sectorsize %u does not match host CPU page size %u, with kernels 6.x and up\n"
"\t the 4KiB sectorsize is supported on all architectures but other combinations\n"
"\t may fail the filesystem mount, use \"--sectorsize %u\" to override that\n",
			sectorsize, page_size, page_size);
	return 0;
}

int btrfs_check_nodesize(u32 nodesize, u32 sectorsize,
			 struct btrfs_mkfs_features *features)
{
	if (nodesize < sectorsize) {
		error("illegal nodesize %u (smaller than %u)",
				nodesize, sectorsize);
		return -1;
	} else if (nodesize > BTRFS_MAX_METADATA_BLOCKSIZE) {
		error("illegal nodesize %u (larger than %u)",
			nodesize, BTRFS_MAX_METADATA_BLOCKSIZE);
		return -1;
	} else if (nodesize & (sectorsize - 1)) {
		error("illegal nodesize %u (not aligned to %u)",
			nodesize, sectorsize);
		return -1;
	} else if (features->incompat_flags &
		   BTRFS_FEATURE_INCOMPAT_MIXED_GROUPS &&
		   nodesize != sectorsize) {
		error(
		"illegal nodesize %u (not equal to %u for mixed block group)",
			nodesize, sectorsize);
		return -1;
	}
	return 0;
}

int btrfs_check_features(const struct btrfs_mkfs_features *features,
			 const struct btrfs_mkfs_features *allowed)
{
	if (features->compat_ro_flags & ~allowed->compat_ro_flags ||
	    features->incompat_flags & ~allowed->incompat_flags ||
	    features->runtime_flags & ~allowed->runtime_flags)
		return -EINVAL;
	return 0;
}

static bool tree_search_v2_supported = false;
static bool tree_search_v2_initialized = false;

/*
 * Call the highest supported TREE_SEARCH ioctl version, autodetect support.
 */
int btrfs_tree_search_ioctl(int fd, struct btrfs_tree_search_args *sa)
{
	/* On first use check the supported status and save it. */
	if (!tree_search_v2_initialized) {
#if 0
		/*
		 * Keep using v1 until v2 is fully tested, in some cases it
		 * does not return properly formatted results in the buffer.
		 */
		if (btrfs_tree_search2_ioctl_supported(fd) == 1)
			tree_search_v2_supported = true;
#endif
		tree_search_v2_initialized = true;
	}
	sa->use_v2 = tree_search_v2_supported;

	if (sa->use_v2) {
		sa->args2.buf_size = BTRFS_TREE_SEARCH_V2_BUF_SIZE;
		return ioctl(fd, BTRFS_IOC_TREE_SEARCH_V2, &sa->args2);
	}
	return ioctl(fd, BTRFS_IOC_TREE_SEARCH, &sa->args1);
}

/*
 * Check if the BTRFS_IOC_TREE_SEARCH_V2 ioctl is supported on a given
 * filesystem, opened at fd
 */
int btrfs_tree_search2_ioctl_supported(int fd)
{
	struct btrfs_ioctl_search_args_v2 *args2;
	struct btrfs_ioctl_search_key *sk;
	int args2_size = 1024;
	char args2_buf[args2_size];
	int ret;

	args2 = (struct btrfs_ioctl_search_args_v2 *)args2_buf;
	sk = &(args2->key);

	/*
	 * Search for the extent tree item in the root tree.
	 */
	sk->tree_id = BTRFS_ROOT_TREE_OBJECTID;
	sk->min_objectid = BTRFS_EXTENT_TREE_OBJECTID;
	sk->min_type = BTRFS_ROOT_ITEM_KEY;
	sk->min_offset = 0;
	sk->max_objectid = BTRFS_EXTENT_TREE_OBJECTID;
	sk->max_type = BTRFS_ROOT_ITEM_KEY;
	sk->max_offset = (u64)-1;
	sk->min_transid = 0;
	sk->max_transid = (u64)-1;
	sk->nr_items = 1;
	args2->buf_size = args2_size - sizeof(struct btrfs_ioctl_search_args_v2);
	ret = ioctl(fd, BTRFS_IOC_TREE_SEARCH_V2, args2);
	if (ret == -EOPNOTSUPP)
		return 0;
	else if (ret == 0)
		return 1;
	return ret;
}

