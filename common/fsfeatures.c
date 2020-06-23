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
#include <linux/version.h>
#include "common/fsfeatures.h"
#include "ctree.h"
#include "common/utils.h"

/*
 * Insert a root item for temporary tree root
 *
 * Only used in make_btrfs_v2().
 */
#define VERSION_TO_STRING3(a,b,c)	#a "." #b "." #c, KERNEL_VERSION(a,b,c)
#define VERSION_TO_STRING2(a,b)		#a "." #b, KERNEL_VERSION(a,b,0)

enum feature_source {
	FS_FEATURES,
	RUNTIME_FEATURES,
};

/*
 * Feature stability status and versions: compat <= safe <= default
 */
struct btrfs_feature {
	const char *name;
	u64 flag;
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

static const struct btrfs_feature mkfs_features[] = {
	{ "mixed-bg", BTRFS_FEATURE_INCOMPAT_MIXED_GROUPS,
		"mixed_groups",
		VERSION_TO_STRING3(2,6,37),
		VERSION_TO_STRING3(2,6,37),
		NULL, 0,
		"mixed data and metadata block groups" },
	{ "extref", BTRFS_FEATURE_INCOMPAT_EXTENDED_IREF,
		"extended_iref",
		VERSION_TO_STRING2(3,7),
		VERSION_TO_STRING2(3,12),
		VERSION_TO_STRING2(3,12),
		"increased hardlink limit per file to 65536" },
	{ "raid56", BTRFS_FEATURE_INCOMPAT_RAID56,
		"raid56",
		VERSION_TO_STRING2(3,9),
		NULL, 0,
		NULL, 0,
		"raid56 extended format" },
	{ "skinny-metadata", BTRFS_FEATURE_INCOMPAT_SKINNY_METADATA,
		"skinny_metadata",
		VERSION_TO_STRING2(3,10),
		VERSION_TO_STRING2(3,18),
		VERSION_TO_STRING2(3,18),
		"reduced-size metadata extent refs" },
	{ "no-holes", BTRFS_FEATURE_INCOMPAT_NO_HOLES,
		"no_holes",
		VERSION_TO_STRING2(3,14),
		VERSION_TO_STRING2(4,0),
		NULL, 0,
		"no explicit hole extents for files" },
	{ "raid1c34", BTRFS_FEATURE_INCOMPAT_RAID1C34,
		"raid1c34",
		VERSION_TO_STRING2(5,5),
		NULL, 0,
		NULL, 0,
		"RAID1 with 3 or 4 copies" },
	/* Keep this one last */
	{ "list-all", BTRFS_FEATURE_LIST_ALL, NULL }
};

static const struct btrfs_feature runtime_features[] = {
	{ "quota", BTRFS_RUNTIME_FEATURE_QUOTA, NULL,
		VERSION_TO_STRING2(3, 4), NULL, 0, NULL, 0,
		"quota support (qgroups)" },
	/* Keep this one last */
	{ "list-all", BTRFS_FEATURE_LIST_ALL, NULL }
};

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

static int parse_one_fs_feature(const char *name, u64 *flags,
				enum feature_source source)
{
	const int array_size = get_feature_array_size(source);
	int i;
	int found = 0;

	for (i = 0; i < array_size; i++) {
		const struct btrfs_feature *feat = get_feature(i, source);

		if (name[0] == '^' && !strcmp(feat->name, name + 1)) {
			*flags &= ~feat->flag;
			found = 1;
		} else if (!strcmp(feat->name, name)) {
			*flags |= feat->flag;
			found = 1;
		}
	}

	return !found;
}

static void parse_features_to_string(char *buf, u64 flags,
				     enum feature_source source)
{
	const int array_size = get_feature_array_size(source);
	int i;

	buf[0] = 0;

	for (i = 0; i < array_size; i++) {
		const struct btrfs_feature *feat = get_feature(i, source);

		if (flags & feat->flag) {
			if (*buf)
				strcat(buf, ", ");
			strcat(buf, feat->name);
		}
	}
}

void btrfs_parse_fs_features_to_string(char *buf, u64 flags)
{
	parse_features_to_string(buf, flags, FS_FEATURES);
}

void btrfs_parse_runtime_features_to_string(char *buf, u64 flags)
{
	parse_features_to_string(buf, flags, RUNTIME_FEATURES);
}

static void process_features(u64 flags, enum feature_source source)
{
	const int array_size = get_feature_array_size(source);
	int i;

	for (i = 0; i < array_size; i++) {
		const struct btrfs_feature *feat = get_feature(i, source);

		if (flags & feat->flag && feat->name && feat->desc) {
			printf("Turning ON incompat feature '%s': %s\n",
				feat->name, feat->desc);
		}
	}
}

void btrfs_process_fs_features(u64 flags)
{
	process_features(flags, FS_FEATURES);
}

void btrfs_process_runtime_features(u64 flags)
{
	process_features(flags, RUNTIME_FEATURES);
}

static void list_all_features(u64 mask_disallowed, enum feature_source source)
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

		if (feat->flag & mask_disallowed)
			continue;
		fprintf(stderr, "%-20s- %s (0x%llx", feat->name, feat->desc,
				feat->flag);
		if (feat->compat_ver)
			fprintf(stderr, ", compat=%s", feat->compat_str);
		if (feat->safe_ver)
			fprintf(stderr, ", safe=%s", feat->safe_str);
		if (feat->default_ver)
			fprintf(stderr, ", default=%s", feat->default_str);
		fprintf(stderr, ")\n");
	}
}

void btrfs_list_all_fs_features(u64 mask_disallowed)
{
	list_all_features(mask_disallowed, FS_FEATURES);
}

void btrfs_list_all_runtime_features(u64 mask_disallowed)
{
	list_all_features(mask_disallowed, RUNTIME_FEATURES);
}

/*
 * Return NULL if all features were parsed fine, otherwise return the name of
 * the first unparsed.
 */
static char *parse_features(char *namelist, u64 *flags,
			    enum feature_source source)
{
	char *this_char;
	char *save_ptr = NULL; /* Satisfy static checkers */

	for (this_char = strtok_r(namelist, ",", &save_ptr);
	     this_char != NULL;
	     this_char = strtok_r(NULL, ",", &save_ptr)) {
		if (parse_one_fs_feature(this_char, flags, source))
			return this_char;
	}

	return NULL;
}

char *btrfs_parse_fs_features(char *namelist, u64 *flags)
{
	return parse_features(namelist, flags, FS_FEATURES);
}

char *btrfs_parse_runtime_features(char *namelist, u64 *flags)
{
	return parse_features(namelist, flags, RUNTIME_FEATURES);
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

int btrfs_check_nodesize(u32 nodesize, u32 sectorsize, u64 features)
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
	} else if (features & BTRFS_FEATURE_INCOMPAT_MIXED_GROUPS &&
		   nodesize != sectorsize) {
		error(
		"illegal nodesize %u (not equal to %u for mixed block group)",
			nodesize, sectorsize);
		return -1;
	}
	return 0;
}
