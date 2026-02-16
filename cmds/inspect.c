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
#include <sys/ioctl.h>
#include <sys/stat.h>
#include <sys/statfs.h>
#include <linux/fs.h>
#include <linux/magic.h>
#include <stdbool.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <errno.h>
#include <getopt.h>
#include <limits.h>
#include <dirent.h>
#include <string.h>
#include <unistd.h>
#include "kernel-lib/list.h"
#include "kernel-lib/sizes.h"
#include "kernel-shared/accessors.h"
#include "kernel-shared/uapi/btrfs_tree.h"
#include "kernel-shared/uapi/btrfs.h"
#include "kernel-shared/ctree.h"
#include "kernel-shared/disk-io.h"
#include "common/internal.h"
#include "common/messages.h"
#include "common/utils.h"
#include "common/send-utils.h"
#include "common/help.h"
#include "common/open-utils.h"
#include "common/units.h"
#include "common/string-utils.h"
#include "common/string-table.h"
#include "common/sort-utils.h"
#include "common/tree-search.h"
#include "cmds/commands.h"

static const char * const inspect_cmd_group_usage[] = {
	"btrfs inspect-internal <command> <args>",
	NULL
};

static int __ino_to_path_fd(u64 inum, int fd, const char *prepend)
{
	int ret;
	int i;
	struct btrfs_ioctl_ino_path_args ipa;
	char pathbuf[PATH_MAX];
	struct btrfs_data_container *fspath = (struct btrfs_data_container *)pathbuf;

	memset(fspath, 0, sizeof(*fspath));
	ipa.inum = inum;
	ipa.size = PATH_MAX;
	ipa.fspath = ptr_to_u64(fspath);

	ret = ioctl(fd, BTRFS_IOC_INO_PATHS, &ipa);
	if (ret < 0) {
		error("ino paths ioctl: %m");
		goto out;
	}

	pr_verbose(LOG_DEBUG,
	"ioctl ret=%d, bytes_left=%lu, bytes_missing=%lu cnt=%d, missed=%d\n",
		   ret, (unsigned long)fspath->bytes_left,
		   (unsigned long)fspath->bytes_missing, fspath->elem_cnt,
		   fspath->elem_missed);

	for (i = 0; i < fspath->elem_cnt; ++i) {
		u64 ptr;
		char *str;
		ptr = (u64)(unsigned long)fspath->val;
		ptr += fspath->val[i];
		str = (char *)(unsigned long)ptr;
		if (prepend)
			pr_verbose(LOG_DEFAULT, "%s/%s\n", prepend, str);
		else
			pr_verbose(LOG_DEFAULT, "%s\n", str);
	}

out:
	return !!ret;
}

static const char * const cmd_inspect_inode_resolve_usage[] = {
	"btrfs inspect-internal inode-resolve [-v] <inode> <path>",
	"Get file system paths for the given inode",
	"",
	OPTLINE("-v", "deprecated, alias for global -v option"),
	HELPINFO_INSERT_GLOBALS,
	HELPINFO_INSERT_VERBOSE,
	NULL
};

static int cmd_inspect_inode_resolve(const struct cmd_struct *cmd,
				     int argc, char **argv)
{
	int fd;
	int ret;

	optind = 0;
	while (1) {
		int c = getopt(argc, argv, "v");
		if (c < 0)
			break;

		switch (c) {
		case 'v':
			bconf_be_verbose();
			break;
		default:
			usage_unknown_option(cmd, argv);
		}
	}

	if (check_argc_exact(argc - optind, 2))
		return 1;

	fd = btrfs_open_dir(argv[optind + 1]);
	if (fd < 0)
		return 1;

	ret = __ino_to_path_fd(arg_strtou64(argv[optind]), fd, argv[optind+1]);
	close(fd);
	return !!ret;

}
static DEFINE_SIMPLE_COMMAND(inspect_inode_resolve, "inode-resolve");

static const char * const cmd_inspect_logical_resolve_usage[] = {
	"btrfs inspect-internal logical-resolve [-Pvo] [-s bufsize] <logical> <path>",
	"Get file system paths for the given logical address",
	"",
	OPTLINE("-P", "skip the path resolving and print the inodes instead"),
	OPTLINE("-o", "ignore offsets when matching references (requires v2 ioctl support in the kernel 4.15+)"),
	OPTLINE("-s bufsize", "set inode container's size. This is used to increase inode "
		"container's size in case it is not enough to read all the "
		"resolved results. The max value one can set is 64k with the "
		"v1 ioctl. Sizes over 64k will use the v2 ioctl (kernel 4.15+)"),
	OPTLINE("-v", "deprecated, alias for global -v option"),
	HELPINFO_INSERT_GLOBALS,
	HELPINFO_INSERT_VERBOSE,
	NULL
};

static int cmd_inspect_logical_resolve(const struct cmd_struct *cmd,
				       int argc, char **argv)
{
	int ret;
	int fd;
	int i;
	bool getpath = true;
	int bytes_left;
	struct btrfs_ioctl_logical_ino_args loi = { 0 };
	struct btrfs_data_container *inodes;
	u64 size = SZ_64K;
	char full_path[PATH_MAX];
	char *path_ptr;
	u64 flags = 0;
	unsigned long request = BTRFS_IOC_LOGICAL_INO;

	optind = 0;
	while (1) {
		int c = getopt(argc, argv, "Pvos:");
		if (c < 0)
			break;

		switch (c) {
		case 'P':
			getpath = false;
			break;
		case 'v':
			bconf_be_verbose();
			break;
		case 'o':
			flags |= BTRFS_LOGICAL_INO_ARGS_IGNORE_OFFSET;
			break;
		case 's':
			size = arg_strtou64(optarg);
			break;
		default:
			usage_unknown_option(cmd, argv);
		}
	}

	if (check_argc_exact(argc - optind, 2))
		return 1;

	size = min(size, (u64)SZ_16M);
	inodes = malloc(size);
	if (!inodes)
		return 1;

	if (size > SZ_64K || flags != 0)
		request = BTRFS_IOC_LOGICAL_INO_V2;

	memset(inodes, 0, sizeof(*inodes));
	loi.logical = arg_strtou64(argv[optind]);
	loi.size = size;
	loi.flags = flags;
	loi.inodes = ptr_to_u64(inodes);

	fd = btrfs_open_dir(argv[optind + 1]);
	if (fd < 0) {
		ret = 12;
		goto out;
	}

	ret = ioctl(fd, request, &loi);
	if (ret < 0) {
		error("logical ino ioctl: %m");
		goto out;
	}

	pr_verbose(LOG_DEBUG,
"ioctl ret=%d, total_size=%llu, bytes_left=%lu, bytes_missing=%lu, cnt=%d, missed=%d\n",
		   ret, size, (unsigned long)inodes->bytes_left,
		   (unsigned long)inodes->bytes_missing, inodes->elem_cnt,
		   inodes->elem_missed);

	bytes_left = sizeof(full_path);
	ret = snprintf(full_path, bytes_left, "%s/", argv[optind+1]);
	path_ptr = full_path + ret;
	bytes_left -= ret + 1;
	if (bytes_left < 0) {
		error("path buffer too small: %d bytes", bytes_left);
		goto out;
	}
	ret = 0;

	for (i = 0; i < inodes->elem_cnt; i += 3) {
		u64 inum = inodes->val[i];
		u64 offset = inodes->val[i+1];
		u64 root = inodes->val[i+2];
		int path_fd;

		if (getpath) {
			char mount_path[PATH_MAX];
			char name[PATH_MAX];

			ret = btrfs_subvolid_resolve(fd, name, sizeof(name), root);
			if (ret < 0)
				goto out;

			if (name[0] == 0) {
				path_ptr[-1] = '\0';
				path_fd = fd;
				strncpy_null(mount_path, full_path, sizeof(mount_path));
			} else {
				char *mounted = NULL;
				char subvol[PATH_MAX];
				char subvolid[PATH_MAX];

				/*
				 * btrfs_subvolid_resolve returns the full
				 * path to the subvolume pointed by root, but the
				 * subvolume can be mounted in a directory name
				 * different from the subvolume name. In this
				 * case we need to find the correct mount point
				 * using same subvolume path and subvol id found
				 * before.
				 */

				snprintf(subvol, PATH_MAX, "/%s", name);
				snprintf(subvolid, PATH_MAX, "%llu", root);

				ret = find_mount_fsroot(subvol, subvolid, &mounted);

				if (ret) {
					error("failed to parse mountinfo");
					goto out;
				}

				if (!mounted) {
					printf(
			"inode %llu subvol %s could not be accessed: not mounted\n",
						inum, name);
					continue;
				}

				strncpy_null(mount_path, mounted, sizeof(mount_path));
				free(mounted);

				path_fd = btrfs_open_dir(mount_path);
				if (path_fd < 0) {
					ret = -ENOENT;
					goto out;
				}
			}
			ret = __ino_to_path_fd(inum, path_fd, mount_path);
			if (path_fd != fd)
				close(path_fd);
		} else {
			pr_verbose(LOG_DEFAULT, "inode %llu offset %llu root %llu\n", inum,
				offset, root);
		}
	}

out:
	close(fd);
	free(inodes);
	return !!ret;
}
static DEFINE_SIMPLE_COMMAND(inspect_logical_resolve, "logical-resolve");

static const char * const cmd_inspect_subvolid_resolve_usage[] = {
	"btrfs inspect-internal subvolid-resolve <subvolid> <path>",
	"Get file system paths for the given subvolume ID.",
	NULL
};

static int cmd_inspect_subvolid_resolve(const struct cmd_struct *cmd,
					int argc, char **argv)
{
	int ret;
	int fd = -1;
	u64 subvol_id;
	char path[PATH_MAX];

	clean_args_no_options(cmd, argc, argv);

	if (check_argc_exact(argc - optind, 2))
		return 1;

	fd = btrfs_open_dir(argv[optind + 1]);
	if (fd < 0) {
		ret = -ENOENT;
		goto out;
	}

	subvol_id = arg_strtou64(argv[optind]);
	ret = btrfs_subvolid_resolve(fd, path, sizeof(path), subvol_id);

	if (ret) {
		error("resolving subvolid %llu error %d", subvol_id, ret);
		goto out;
	}

	path[PATH_MAX - 1] = '\0';
	pr_verbose(LOG_DEFAULT, "%s\n", path);

out:
	close(fd);
	return !!ret;
}
static DEFINE_SIMPLE_COMMAND(inspect_subvolid_resolve, "subvolid-resolve");

static const char* const cmd_inspect_rootid_usage[] = {
	"btrfs inspect-internal rootid <path>",
	"Get tree ID of the containing subvolume of path.",
	NULL
};

static int cmd_inspect_rootid(const struct cmd_struct *cmd,
			      int argc, char **argv)
{
	int ret;
	int fd = -1;
	u64 rootid;

	clean_args_no_options(cmd, argc, argv);

	if (check_argc_exact(argc - optind, 1))
		return 1;

	fd = btrfs_open_file_or_dir(argv[optind]);
	if (fd < 0) {
		ret = -ENOENT;
		goto out;
	}

	ret = lookup_path_rootid(fd, &rootid);
	if (ret) {
		errno = -ret;
		error("failed to lookup root id: %m");
		goto out;
	}

	pr_verbose(LOG_DEFAULT, "%llu\n", rootid);
out:
	close(fd);

	return !!ret;
}
static DEFINE_SIMPLE_COMMAND(inspect_rootid, "rootid");

static const char* const cmd_inspect_min_dev_size_usage[] = {
	"btrfs inspect-internal min-dev-size [options] <path>",
	"Get the minimum size the device can be shrunk to",
	"",
	"The device id 1 is used by default.",
	OPTLINE("--id DEVID", "specify the device id to query"),
	NULL
};

struct dev_extent_elem {
	u64 start;
	/* inclusive end */
	u64 end;
	struct list_head list;
};

static int add_dev_extent(struct list_head *list,
			  const u64 start, const u64 end,
			  const int append)
{
	struct dev_extent_elem *e;

	e = malloc(sizeof(*e));
	if (!e)
		return -ENOMEM;

	e->start = start;
	e->end = end;

	if (append)
		list_add_tail(&e->list, list);
	else
		list_add(&e->list, list);

	return 0;
}

static void free_dev_extent_list(struct list_head *list)
{
	while (!list_empty(list)) {
		struct dev_extent_elem *e;

		e = list_first_entry(list, struct dev_extent_elem, list);
		list_del(&e->list);
		free(e);
	}
}

static int hole_includes_sb_mirror(const u64 start, const u64 end)
{
	int i;
	int ret = 0;

	for (i = 0; i < BTRFS_SUPER_MIRROR_MAX; i++) {
		u64 bytenr = btrfs_sb_offset(i);

		if (bytenr >= start && bytenr <= end) {
			ret = 1;
			break;
		}
	}

	return ret;
}

static void adjust_dev_min_size(struct list_head *extents,
				struct list_head *holes,
				u64 *min_size)
{
	/*
	 * If relocation of the block group of a device extent must happen (see
	 * below) scratch space is used for the relocation. So track here the
	 * size of the largest device extent that has to be relocated. We track
	 * only the largest and not the sum of the sizes of all relocated block
	 * groups because after each block group is relocated the running
	 * transaction is committed so that pinned space is released.
	 */
	u64 scratch_space = 0;

	/*
	 * List of device extents is sorted by descending order of the extent's
	 * end offset. If some extent goes beyond the computed minimum size,
	 * which initially matches the sum of the lengths of all extents,
	 * we need to check if the extent can be relocated to an hole in the
	 * device between [0, *min_size[ (which is what the resize ioctl does).
	 */
	while (!list_empty(extents)) {
		struct dev_extent_elem *e;
		struct dev_extent_elem *h;
		int found = 0;
		u64 extent_len;
		u64 hole_len = 0;

		e = list_first_entry(extents, struct dev_extent_elem, list);
		if (e->end <= *min_size)
			break;

		/*
		 * Our extent goes beyond the computed *min_size. See if we can
		 * find a hole large enough to relocate it to. If not we must stop
		 * and set *min_size to the end of the extent.
		 */
		extent_len = e->end - e->start + 1;
		list_for_each_entry(h, holes, list) {
			hole_len = h->end - h->start + 1;
			if (hole_len >= extent_len) {
				found = 1;
				break;
			}
		}

		if (!found) {
			*min_size = e->end + 1;
			break;
		}

		/*
		 * If the hole found contains the location for a superblock
		 * mirror, we are pessimistic and require allocating one
		 * more extent of the same size. This is because the block
		 * group could be in the worst case used by a single extent
		 * with a size >= (block_group.length - superblock.size).
		 */
		if (hole_includes_sb_mirror(h->start,
					    h->start + extent_len - 1))
			*min_size += extent_len;

		if (hole_len > extent_len) {
			h->start += extent_len;
		} else {
			list_del(&h->list);
			free(h);
		}

		list_del(&e->list);
		free(e);

		if (extent_len > scratch_space)
			scratch_space = extent_len;
	}

	if (scratch_space) {
		*min_size += scratch_space;
		/*
		 * Chunk allocation requires inserting/updating items in the
		 * chunk tree, so often this can lead to the need of allocating
		 * a new system chunk too, which has a maximum size of 32Mb.
		 */
		*min_size += SZ_32M;
	}
}

static int print_min_dev_size(int fd, u64 devid)
{
	int ret = 1;
	/*
	 * Device allocations starts at 1Mb or at the value passed through the
	 * mount option alloc_start if it's bigger than 1Mb. The alloc_start
	 * option is used for debugging and testing only, and recently the
	 * possibility of deprecating/removing it has been discussed, so we
	 * ignore it here.
	 */
	u64 min_size = SZ_1M;
	struct btrfs_tree_search_args args;
	struct btrfs_ioctl_search_key *sk;
	u64 last_pos = (u64)-1;
	LIST_HEAD(extents);
	LIST_HEAD(holes);

	memset(&args, 0, sizeof(args));
	sk = btrfs_tree_search_sk(&args);
	sk->tree_id = BTRFS_DEV_TREE_OBJECTID;
	sk->min_objectid = devid;
	sk->min_type = BTRFS_DEV_EXTENT_KEY;
	sk->min_offset = 0;
	sk->max_objectid = devid;
	sk->max_type = BTRFS_DEV_EXTENT_KEY;
	sk->max_offset = (u64)-1;
	sk->min_transid = 0;
	sk->max_transid = (u64)-1;
	sk->nr_items = 4096;

	while (1) {
		int i;
		unsigned long off = 0;

		ret = btrfs_tree_search_ioctl(fd, &args);
		if (ret < 0) {
			error("tree search ioctl: %m");
			ret = 1;
			goto out;
		}

		if (sk->nr_items == 0)
			break;

		for (i = 0; i < sk->nr_items; i++) {
			struct btrfs_dev_extent *extent;
			struct btrfs_ioctl_search_header sh;
			u64 len;

			memcpy(&sh, btrfs_tree_search_data(&args, off), sizeof(sh));
			off += sizeof(sh);
			extent = btrfs_tree_search_data(&args, off);
			off += sh.len;

			sk->min_objectid = sh.objectid;
			sk->min_type = sh.type;
			sk->min_offset = sh.offset + 1;

			if (sh.objectid != devid || sh.type != BTRFS_DEV_EXTENT_KEY)
				continue;

			len = btrfs_stack_dev_extent_length(extent);
			min_size += len;
			ret = add_dev_extent(&extents, sh.offset,
					     sh.offset + len - 1, 0);

			if (!ret && last_pos != (u64)-1 && last_pos != sh.offset)
				ret = add_dev_extent(&holes, last_pos,
						     sh.offset - 1, 1);
			if (ret) {
				errno = -ret;
				error("add device extent: %m");
				ret = 1;
				goto out;
			}

			last_pos = sh.offset + len;
		}

		if (sk->min_type != BTRFS_DEV_EXTENT_KEY ||
		    sk->min_objectid != devid)
			break;
	}

	adjust_dev_min_size(&extents, &holes, &min_size);
	pr_verbose(LOG_DEFAULT, "%llu bytes (%s)\n", min_size, pretty_size(min_size));
	ret = 0;
out:
	free_dev_extent_list(&extents);
	free_dev_extent_list(&holes);

	return ret;
}

static int cmd_inspect_min_dev_size(const struct cmd_struct *cmd,
				    int argc, char **argv)
{
	int ret;
	int fd = -1;
	u64 devid = 1;

	optind = 0;
	while (1) {
		int c;
		enum { GETOPT_VAL_DEVID = GETOPT_VAL_FIRST };
		static const struct option long_options[] = {
			{ "id", required_argument, NULL, GETOPT_VAL_DEVID },
			{NULL, 0, NULL, 0}
		};

		c = getopt_long(argc, argv, "", long_options, NULL);
		if (c < 0)
			break;

		switch (c) {
		case GETOPT_VAL_DEVID:
			devid = arg_strtou64(optarg);
			break;
		default:
			usage_unknown_option(cmd, argv);
		}
	}
	if (check_argc_exact(argc - optind, 1))
		return 1;

	fd = btrfs_open_dir(argv[optind]);
	if (fd < 0) {
		ret = -ENOENT;
		goto out;
	}

	ret = print_min_dev_size(fd, devid);
	close(fd);
out:
	return !!ret;
}
static DEFINE_SIMPLE_COMMAND(inspect_min_dev_size, "min-dev-size");

static const char * const cmd_inspect_list_chunks_usage[] = {
	"btrfs inspect-internal list-chunks [options] <path>",
	"Enumerate chunks on all devices",
	"Enumerate chunks on all devices. Chunks are the physical storage tied to a device,",
	"striped profiles they appear multiple times for a given logical offset, on other",
	"profiles the correspondence is 1:1 or 1:N.",
	"",
	HELPINFO_UNITS_LONG,
	OPTLINE("--sort MODE", "sort by a column (ascending, prepend '-' for descending):\n"
			"MODE is a coma separated list of:\n"
			"devid - by device id (default, with pstart)\n"
			"pstart - physical start\n"
			"lstart - logical offset\n"
			"usage  - by chunk usage\n"
			"length - by chunk length\n"
			"type   - chunk type (data, metadata, system)\n"
			"profile - chunk profile (single, RAID, ...)"
	       ),
	NULL
};

struct list_chunks_entry {
	u64 devid;
	u64 start;
	u64 lstart;
	u64 length;
	u64 flags;
	u64 lnumber;
	u64 used;
	u64 pnumber;
};

struct list_chunks_ctx {
	unsigned length;
	unsigned size;
	struct list_chunks_entry *stats;
};

static int cmp_cse_devid(const void *va, const void *vb)
{
	const struct list_chunks_entry *a = va;
	const struct list_chunks_entry *b = vb;

	if (a->devid < b->devid)
		return -1;
	if (a->devid > b->devid)
		return 1;
	return 0;
}

static int cmp_cse_devid_pstart(const void *va, const void *vb)
{
	const struct list_chunks_entry *a = va;
	const struct list_chunks_entry *b = vb;

	if (a->devid < b->devid)
		return -1;
	if (a->devid > b->devid)
		return 1;

	if (a->start < b->start)
		return -1;
	if (a->start == b->start)
		return 0;
	return 1;
}

static int cmp_cse_pstart(const void *va, const void *vb)
{
	const struct list_chunks_entry *a = va;
	const struct list_chunks_entry *b = vb;

	if (a->start < b->start)
		return -1;
	if (a->start == b->start)
		return 0;
	return 1;
}

static int cmp_cse_lstart(const void *va, const void *vb)
{
	const struct list_chunks_entry *a = va;
	const struct list_chunks_entry *b = vb;

	if (a->lstart < b->lstart)
		return -1;
	if (a->lstart == b->lstart)
		return 0;
	return 1;
}

/* Compare entries by usage percent, descending. */
static int cmp_cse_usage(const void *va, const void *vb)
{
	const struct list_chunks_entry *a = va;
	const struct list_chunks_entry *b = vb;
	const float usage_a = (float)a->used / a->length * 100;
	const float usage_b = (float)b->used / b->length * 100;
	const float epsilon = 1e-8;

	if (usage_b - usage_a < epsilon)
		return 1;
	if (usage_a - usage_b < epsilon)
		return -1;
	return 0;
}

static int cmp_cse_length(const void *va, const void *vb)
{
	const struct list_chunks_entry *a = va;
	const struct list_chunks_entry *b = vb;

	if (a->length < b->length)
		return -1;
	if (a->length > b->length)
		return 1;
	return 0;
}

static int cmp_cse_ch_type(const void *va, const void *vb)
{
	const struct list_chunks_entry *a = va;
	const struct list_chunks_entry *b = vb;
	const u64 atype = (a->flags & BTRFS_BLOCK_GROUP_TYPE_MASK);
	const u64 btype = (b->flags & BTRFS_BLOCK_GROUP_TYPE_MASK);
	static int order[] = {
		[BTRFS_BLOCK_GROUP_DATA]     = 0,
		[BTRFS_BLOCK_GROUP_METADATA] = 1,
		[BTRFS_BLOCK_GROUP_SYSTEM]   = 2
	};

	if (order[atype] < order[btype])
		return -1;
	if (order[atype] > order[btype])
		return 1;
	return 0;
}

static int cmp_cse_ch_profile(const void *va, const void *vb)
{
	const struct list_chunks_entry *a = va;
	const struct list_chunks_entry *b = vb;
	const u64 aprofile = (a->flags & BTRFS_BLOCK_GROUP_PROFILE_MASK);
	const u64 bprofile = (b->flags & BTRFS_BLOCK_GROUP_PROFILE_MASK);
	static int order[] = {
		[0 /* single */]            = 0,
		[BTRFS_BLOCK_GROUP_DUP]     = 1,
		[BTRFS_BLOCK_GROUP_RAID0]   = 2,
		[BTRFS_BLOCK_GROUP_RAID1]   = 3,
		[BTRFS_BLOCK_GROUP_RAID1C3] = 4,
		[BTRFS_BLOCK_GROUP_RAID1C4] = 5,
		[BTRFS_BLOCK_GROUP_RAID10]  = 6,
		[BTRFS_BLOCK_GROUP_RAID5]   = 7,
		[BTRFS_BLOCK_GROUP_RAID6]   = 8,
	};

	if (order[aprofile] < order[bprofile])
		return -1;
	if (order[aprofile] > order[bprofile])
		return 1;
	return 0;
}

static int print_list_chunks(struct list_chunks_ctx *ctx, const char *sortmode,
			     unsigned unit_mode)
{
	u64 devid;
	struct list_chunks_entry e;
	struct string_table *table;
	int col_count, col;
	int i;
	u64 number;
	u32 tabidx;
	enum {
		CHUNK_SORT_PSTART,
		CHUNK_SORT_LSTART,
		CHUNK_SORT_USAGE,
		CHUNK_SORT_LENGTH,
		CHUNK_SORT_CH_TYPE,
		CHUNK_SORT_CH_PROFILE,
		CHUNK_SORT_DEFAULT = CHUNK_SORT_PSTART
	};
	static const struct sortdef sortit[] = {
		{ .name = "devid", .comp = (sort_cmp_t)cmp_cse_devid,
		  .desc = "sort by device id (default, with pstart)",
		  .id = CHUNK_SORT_PSTART
		},
		{ .name = "pstart", .comp = (sort_cmp_t)cmp_cse_pstart,
		  .desc = "sort by physical start offset",
		  .id = CHUNK_SORT_PSTART
		},
		{ .name = "lstart", .comp = (sort_cmp_t)cmp_cse_lstart,
		  .desc = "sort by logical offset",
		  .id = CHUNK_SORT_LSTART
		},
		{ .name = "usage", .comp = (sort_cmp_t)cmp_cse_usage,
		  .desc = "sort by chunk usage",
		  .id = CHUNK_SORT_USAGE
		},
		{ .name = "length", .comp = (sort_cmp_t)cmp_cse_length,
		  .desc = "sort by length",
		  .id = CHUNK_SORT_LENGTH
		},
		{ .name = "type", .comp = (sort_cmp_t)cmp_cse_ch_type,
		  .desc = "sort by chunk type",
		  .id = CHUNK_SORT_CH_TYPE
		},
		{ .name = "profile", .comp = (sort_cmp_t)cmp_cse_ch_profile,
		  .desc = "sort by chunk profile",
		  .id = CHUNK_SORT_CH_PROFILE
		},
		SORTDEF_END
	};
	const char *tmp;
	struct compare comp;
	int id;

	compare_init(&comp, sortit);

	tmp = sortmode;
	do {
		bool descending;

		id = compare_parse_key_to_id(&comp, &tmp, &descending);
		if (id == -1) {
			error("unknown sort key: %s", tmp);
			return 1;
		}
		compare_add_sort_id(&comp, id, descending);
	} while (id >= 0);

	/*
	 * Chunks are sorted logically as found by the ioctl, we need to sort
	 * them once to find the physical ordering. This is the default mode.
	 */
	qsort(ctx->stats, ctx->length, sizeof(ctx->stats[0]), cmp_cse_devid_pstart);
	devid = 0;
	number = 0;
	for (i = 0; i < ctx->length; i++) {
		e = ctx->stats[i];
		if (e.devid != devid) {
			devid = e.devid;
			number = 0;
		}
		ctx->stats[i].pnumber = number++;
	}

	/* Skip additional sort if nothing defined by user. */
	if (comp.count > 0)
		qsort_r(ctx->stats, ctx->length, sizeof(ctx->stats[0]),
			(sort_r_cmp_t)compare_cmp_multi, &comp);

	col_count = 9;
	/* Two rows for header and separator. */
	table = table_create(col_count, 2 + ctx->length);
	if (!table) {
		error_mem(NULL);
		return 1;
	}
	/* Print header */
	col = 0;
	tabidx = 0;
	table_printf(table, col++, tabidx, ">Devid");
	table_printf(table, col++, tabidx, ">PNumber");
	table_printf(table, col++, tabidx, ">Type/profile");
	table_printf(table, col++, tabidx, ">PStart");
	table_printf(table, col++, tabidx, ">Length");
	table_printf(table, col++, tabidx, ">PEnd");
	table_printf(table, col++, tabidx, ">LNumber");
	table_printf(table, col++, tabidx, ">LStart");
	table_printf(table, col++, tabidx, ">Usage%%");
	for (int j = 0; j < col_count; j++)
		table_printf(table, j, tabidx + 1, "*-");

	tabidx = 2;
	devid = 0;
	for (i = 0; i < ctx->length; i++) {
		e = ctx->stats[i];
		if (e.devid != devid)
			devid = e.devid;
		col = 0;
		table_printf(table, col++, tabidx, ">%llu", e.devid);
		table_printf(table, col++, tabidx, ">%llu", e.pnumber + 1);
		table_printf(table, col++, tabidx, ">%10s/%-6s",
				btrfs_group_type_str(e.flags),
				btrfs_group_profile_str(e.flags));
		table_printf(table, col++, tabidx, ">%s", pretty_size_mode(e.start, unit_mode));
		table_printf(table, col++, tabidx, ">%s", pretty_size_mode(e.length, unit_mode));
		table_printf(table, col++, tabidx, ">%s", pretty_size_mode(e.start + e.length, unit_mode));
		table_printf(table, col++, tabidx, ">%llu", e.lnumber + 1);
		table_printf(table, col++, tabidx, ">%s", pretty_size_mode(e.lstart, unit_mode));
		table_printf(table, col++, tabidx, ">%6.2f", (float)e.used / e.length * 100);
		tabidx++;
	}
	table_dump(table);
	table_free(table);

	return 0;
}

static u64 fill_usage(int fd, u64 lstart)
{
	struct btrfs_tree_search_args args;
	struct btrfs_ioctl_search_key *sk;
	struct btrfs_block_group_item *item;
	int ret;

	memset(&args, 0, sizeof(args));
	sk = btrfs_tree_search_sk(&args);
	sk->tree_id = BTRFS_EXTENT_TREE_OBJECTID;
	sk->min_objectid = lstart;
	sk->min_type = BTRFS_BLOCK_GROUP_ITEM_KEY;
	sk->min_offset = 0;
	sk->max_objectid = lstart;
	sk->max_type = BTRFS_BLOCK_GROUP_ITEM_KEY;
	sk->max_offset = (u64)-1;
	sk->max_transid = (u64)-1;
	sk->nr_items = 1;

	ret = btrfs_tree_search_ioctl(fd, &args);
	if (ret < 0) {
		error("cannot perform the search: %m");
		return 1;
	}
	if (sk->nr_items == 0) {
		warning("blockgroup %llu not found", lstart);
		return 0;
	}
	if (sk->nr_items > 1)
		warning("found more than one blockgroup %llu", lstart);

	/* Only one item, we don't need search header. */
	item = btrfs_tree_search_data(&args, sizeof(struct btrfs_ioctl_search_header));

	return item->used;
}

static int cmd_inspect_list_chunks(const struct cmd_struct *cmd,
				   int argc, char **argv)
{
	struct btrfs_tree_search_args args;
	struct btrfs_ioctl_search_key *sk;
	unsigned long off = 0;
	u64 *lnumber = NULL;
	unsigned lnumber_size = 128;
	int ret;
	int fd;
	int i;
	unsigned unit_mode;
	char *sortmode = NULL;
	const char *path;
	struct list_chunks_ctx ctx = {
		.length = 0,
		.size = 1024,
		.stats = NULL
	};

	unit_mode = get_unit_mode_from_arg(&argc, argv, 0);

	while (1) {
		int c;
		enum { GETOPT_VAL_SORT = GETOPT_VAL_FIRST,
		       GETOPT_VAL_EMPTY, GETOPT_VAL_NO_EMPTY
		};
		static const struct option long_options[] = {
			{"sort", required_argument, NULL, GETOPT_VAL_SORT },
			{NULL, 0, NULL, 0}
		};

		c = getopt_long(argc, argv, "", long_options, NULL);
		if (c < 0)
			break;

		switch (c) {
		case GETOPT_VAL_SORT:
			free(sortmode);
			sortmode = strdup(optarg);
			break;
		default:
			usage_unknown_option(cmd, argv);
		}
	}

	if (check_argc_exact(argc - optind, 1))
		return 1;

	ctx.stats = calloc(ctx.size, sizeof(ctx.stats[0]));
	if (!ctx.stats) {
		ret = 1;
		error_mem(NULL);
		goto out;
	}

	path = argv[optind];

	fd = btrfs_open_file_or_dir(path);
	if (fd < 0) {
		ret = 1;
		goto out;
	}

	memset(&args, 0, sizeof(args));
	sk = btrfs_tree_search_sk(&args);
	sk->tree_id = BTRFS_CHUNK_TREE_OBJECTID;
	sk->min_objectid = BTRFS_FIRST_CHUNK_TREE_OBJECTID;
	sk->min_type = BTRFS_CHUNK_ITEM_KEY;
	sk->max_objectid = BTRFS_FIRST_CHUNK_TREE_OBJECTID;
	sk->max_type = BTRFS_CHUNK_ITEM_KEY;
	sk->max_offset = (u64)-1;
	sk->max_transid = (u64)-1;
	lnumber = calloc(lnumber_size, sizeof(u64));
	if (!lnumber) {
		ret = 1;
		error_mem(NULL);
		goto out;
	}

	while (1) {
		sk->nr_items = 1;
		ret = btrfs_tree_search_ioctl(fd, &args);
		if (ret < 0) {
			error("cannot perform the search: %m");
			goto out;
		}
		if (sk->nr_items == 0)
			break;

		off = 0;
		for (i = 0; i < sk->nr_items; i++) {
			struct btrfs_chunk *item;
			struct btrfs_stripe *stripes;
			struct btrfs_ioctl_search_header sh;
			int sidx;
			u64 used = (u64)-1;

			memcpy(&sh, btrfs_tree_search_data(&args, off), sizeof(sh));
			off += sizeof(sh);
			item = btrfs_tree_search_data(&args, off);
			off += sh.len;

			stripes = &item->stripe;
			for (sidx = 0; sidx < item->num_stripes; sidx++) {
				struct list_chunks_entry *e;
				u64 devid;

				e = &ctx.stats[ctx.length];
				devid = stripes[sidx].devid;
				e->devid = devid;
				e->start = stripes[sidx].offset;
				e->lstart = sh.offset;
				e->length = item->length;
				e->flags = item->type;
				e->pnumber = -1;
				while (devid > lnumber_size) {
					u64 *tmp;
					unsigned old_size = lnumber_size;

					lnumber_size += 128;
					tmp = calloc(lnumber_size, sizeof(u64));
					if (!tmp) {
						ret = 1;
						error_mem(NULL);
						goto out;
					}
					memcpy(tmp, lnumber, sizeof(u64) * old_size);
					lnumber = tmp;
				}
				e->lnumber = lnumber[devid]++;
				if (used == (u64)-1)
					used = fill_usage(fd, sh.offset);
				e->used = used;

				ctx.length++;

				if (ctx.length == ctx.size) {
					ctx.size += 1024;
					ctx.stats = realloc(ctx.stats, ctx.size
						* sizeof(ctx.stats[0]));
					if (!ctx.stats) {
						ret = 1;
						error_mem(NULL);
						goto out;
					}
				}
			}

			sk->min_objectid = sh.objectid;
			sk->min_type = sh.type;
			sk->min_offset = sh.offset;
		}
		if (sk->min_offset < (u64)-1)
			sk->min_offset++;
		else
			break;
	}

	ret = print_list_chunks(&ctx, sortmode, unit_mode);
	close(fd);

out:
	free(ctx.stats);
	free(lnumber);

	return !!ret;
}
static DEFINE_SIMPLE_COMMAND(inspect_list_chunks, "list-chunks");

static const char * const cmd_inspect_map_swapfile_usage[] = {
	"btrfs inspect-internal map-swapfile <file>",
	"Print physical offset of first block and resume offset if file is suitable as swapfile",
	"Print physical offset of first block and resume offset if file is suitable as swapfile.",
	"All conditions of a swapfile extents are verified if they could pass kernel tests.",
	"Use the value of resume offset for /sys/power/resume_offset, this depends on the",
	"page size that's detected on this system.",
	"",
	OPTLINE("-r|--resume-offset", "print only the value of resume_offset"),
	NULL
};

struct stripe {
	u64 devid;
	u64 offset;
};

struct chunk {
	u64 offset;
	u64 length;
	u64 stripe_len;
	u64 type;
	struct stripe *stripes;
	size_t num_stripes;
	size_t sub_stripes;
};

struct chunk_tree {
	struct chunk *chunks;
	size_t num_chunks;
};

static int read_chunk_tree(int fd, struct chunk **chunks, size_t *num_chunks)
{
	struct btrfs_tree_search_args args;
	struct btrfs_ioctl_search_key *sk;
	size_t items_pos = 0, buf_off = 0;
	size_t capacity = 0;
	int ret;

	memset(&args, 0, sizeof(args));
	sk = btrfs_tree_search_sk(&args);
	sk->tree_id = BTRFS_CHUNK_TREE_OBJECTID;
	sk->min_objectid = BTRFS_FIRST_CHUNK_TREE_OBJECTID;
	sk->min_type = BTRFS_CHUNK_ITEM_KEY;
	sk->min_offset = 0;
	sk->max_objectid = BTRFS_FIRST_CHUNK_TREE_OBJECTID;
	sk->max_type = BTRFS_CHUNK_ITEM_KEY;
	sk->max_offset = (u64)-1;
	sk->min_transid = 0;
	sk->max_transid = (u64)-1;
	sk->nr_items = 0;

	*chunks = NULL;
	*num_chunks = 0;
	for (;;) {
		struct btrfs_ioctl_search_header sh;
		const struct btrfs_chunk *item;
		struct chunk *chunk;
		size_t i;

		if (items_pos >= sk->nr_items) {
			sk->nr_items = 4096;
			ret = btrfs_tree_search_ioctl(fd, &args);
			if (ret == -1) {
				perror("BTRFS_IOC_TREE_SEARCH");
				return -1;
			}
			items_pos = 0;
			buf_off = 0;

			if (sk->nr_items == 0)
				break;
		}

		memcpy(&sh, btrfs_tree_search_data(&args, buf_off), sizeof(sh));
		buf_off += sizeof(sh);

		if (sh.type != BTRFS_CHUNK_ITEM_KEY)
			goto next;

		item = btrfs_tree_search_data(&args, buf_off);
		if (*num_chunks >= capacity) {
			struct chunk *tmp;

			if (capacity == 0)
				capacity = 1;
			else
				capacity *= 2;
			tmp = realloc(*chunks, capacity * sizeof(**chunks));
			if (!tmp) {
				perror("realloc");
				return -1;
			}
			*chunks = tmp;
		}

		chunk = &(*chunks)[*num_chunks];
		chunk->offset = sh.offset;
		chunk->length = get_unaligned_le64(&item->length);
		chunk->stripe_len = get_unaligned_le64(&item->stripe_len);
		chunk->type = get_unaligned_le64(&item->type);
		chunk->num_stripes = get_unaligned_le16(&item->num_stripes);
		chunk->sub_stripes = get_unaligned_le16(&item->sub_stripes);
		chunk->stripes = calloc(chunk->num_stripes, sizeof(*chunk->stripes));
		if (!chunk->stripes) {
			perror("calloc");
			return -1;
		}
		(*num_chunks)++;

		for (i = 0; i < chunk->num_stripes; i++) {
			const struct btrfs_stripe *stripe;

			stripe = &item->stripe + i;
			chunk->stripes[i].devid = get_unaligned_le64(&stripe->devid);
			chunk->stripes[i].offset = get_unaligned_le64(&stripe->offset);
		}

next:
		items_pos++;
		buf_off += sh.len;
		if (sh.offset == (u64)-1)
			break;
		else
			sk->min_offset = sh.offset + 1;
	}
	return 0;
}

static struct chunk *find_chunk(struct chunk *chunks, size_t num_chunks, u64 logical)
{
	size_t lo, hi;

	if (!num_chunks)
		return NULL;

	lo = 0;
	hi = num_chunks - 1;
	while (lo <= hi) {
		size_t mid = lo + (hi - lo) / 2;

		if (logical < chunks[mid].offset)
			hi = mid - 1;
		else if (logical >= chunks[mid].offset + chunks[mid].length)
			lo = mid + 1;
		else
			return &chunks[mid];
	}
	return NULL;
}

static int map_physical_start(int fd, struct chunk *chunks, size_t num_chunks,
			      u64 *physical_start)
{
	struct btrfs_tree_search_args args;
	struct btrfs_ioctl_search_key *sk;
	struct btrfs_ioctl_ino_lookup_args ino_args = {
		.treeid = 0,
		.objectid = BTRFS_FIRST_FREE_OBJECTID,
	};
	size_t items_pos = 0, buf_off = 0;
	struct stat st;
	int ret;
	u64 valid_devid = (u64)-1;

	*physical_start = (u64)-1;

	ret = fstat(fd, &st);
	if (ret == -1) {
		error("cannot fstat file: %m");
		return -errno;
	}
	if (!S_ISREG(st.st_mode)) {
		error("not a regular file");
		return -EINVAL;
	}

	ret = ioctl(fd, BTRFS_IOC_INO_LOOKUP, &ino_args);
	if (ret == -1) {
		error("cannot lookup parent subvolume: %m");
		return -errno;
	}

	memset(&args, 0, sizeof(args));
	sk = btrfs_tree_search_sk(&args);
	sk->min_type = BTRFS_EXTENT_DATA_KEY;
	sk->max_type = BTRFS_EXTENT_DATA_KEY;
	sk->min_offset = 0;
	sk->max_offset = (u64)-1;
	sk->min_transid = 0;
	sk->max_transid = (u64)-1;
	sk->nr_items = 0;
	sk->tree_id = ino_args.treeid;
	sk->min_objectid = sk->max_objectid = st.st_ino;
	for (;;) {
		struct btrfs_ioctl_search_header sh;
		const struct btrfs_file_extent_item *item;
		u8 type;
		u64 logical_offset = 0;
		struct chunk *chunk = NULL;

		if (items_pos >= sk->nr_items) {
			sk->nr_items = 4096;
			ret = btrfs_tree_search_ioctl(fd, &args);
			if (ret == -1) {
				error("cannot search tree: %m");
				return -errno;
			}
			items_pos = 0;
			buf_off = 0;

			if (sk->nr_items == 0)
				break;
		}

		memcpy(&sh, btrfs_tree_search_data(&args, buf_off), sizeof(sh));
		buf_off += sizeof(sh);

		if (sh.type != BTRFS_EXTENT_DATA_KEY)
			goto next;

		item = btrfs_tree_search_data(&args, buf_off);

		type = item->type;
		if (type == BTRFS_FILE_EXTENT_REG ||
		    type == BTRFS_FILE_EXTENT_PREALLOC) {
			logical_offset = get_unaligned_le64(&item->disk_bytenr);
			if (logical_offset) {
				/* Regular extent */
				chunk = find_chunk(chunks, num_chunks, logical_offset);
				if (!chunk) {
					error("cannot find chunk containing %llu",
						logical_offset);
					return -ENOENT;
				}
			} else {
				error("file with holes");
				ret = -EINVAL;
				goto out;
			}
		} else {
			if (type == BTRFS_FILE_EXTENT_INLINE)
				error("file with inline extent");
			else
				error("unknown extent type: %u", type);
			ret = -EINVAL;
			goto out;
		}

		if (item->compression != 0) {
			error("compressed extent: %u", item->compression);
			ret = -EINVAL;
			goto out;
		}
		if (item->encryption != 0) {
			error("file with encryption: %u", item->encryption);
			ret = -EINVAL;
			goto out;
		}
		if (item->other_encoding != 0) {
			error("file with other_encoding: %u", get_unaligned_le16(&item->other_encoding));
			ret = -EINVAL;
			goto out;
		}

		/* Only single profile */
		if ((chunk->type & BTRFS_BLOCK_GROUP_PROFILE_MASK) != 0) {
			error("unsupported block group profile: %llu",
				chunk->type & BTRFS_BLOCK_GROUP_PROFILE_MASK);
			ret = -EINVAL;
			goto out;
		}

		if (valid_devid == (u64)-1) {
			valid_devid = chunk->stripes[0].devid;
		} else {
			if (valid_devid != chunk->stripes[0].devid) {
				error("file stored on multiple devices");
				break;
			}
		}
		if (*physical_start == (u64)-1) {
			u64 offset;
			u64 stripe_nr;
			u64 stripe_offset;
			u64 stripe_index;

			offset = logical_offset - chunk->offset;
			stripe_nr = offset / chunk->stripe_len;
			stripe_offset = offset - stripe_nr * chunk->stripe_len;

			stripe_index = stripe_nr % chunk->num_stripes;
			stripe_nr /= chunk->num_stripes;

			*physical_start = chunk->stripes[stripe_index].offset +
				stripe_nr * chunk->stripe_len + stripe_offset;
		}

next:
		items_pos++;
		buf_off += sh.len;
		if (sh.offset == (u64)-1)
			break;
		else
			sk->min_offset = sh.offset + 1;
	}
	return 0;

out:
	return ret;
}

static int cmd_inspect_map_swapfile(const struct cmd_struct *cmd,
				    int argc, char **argv)
{
	int fd, ret;
	struct chunk *chunks = NULL;
	size_t num_chunks = 0, i;
	struct statfs stfs;
	long flags;
	u64 physical_start;
	long page_size = sysconf(_SC_PAGESIZE);
	bool resume_offset = false;

	optind = 0;
	while (1) {
		static const struct option long_options[] = {
			{ "resume-offset", no_argument, NULL, 'r' },
			{ NULL, 0, NULL, 0 }
		};
		int c;

		c = getopt_long(argc, argv, "r", long_options, NULL);
		if (c == -1)
			break;

		switch (c) {
		case 'r':
			resume_offset = true;
			break;
		default:
			usage_unknown_option(cmd, argv);
		}
	}
	if (check_argc_exact(argc - optind, 1))
		return 1;

	fd = open(argv[optind], O_RDONLY);
	if (fd == -1) {
		error("cannot open %s: %m", argv[optind]);
		ret = 1;
		goto out;
	}

	/* Quick checks before extent enumeration. */
	ret = fstatfs(fd, &stfs);
	if (ret == -1) {
		error("cannot statfs file: %m");
		ret = 1;
		goto out;
	}
	if (stfs.f_type != BTRFS_SUPER_MAGIC) {
		error("not a file on BTRFS");
		ret = 1;
		goto out;
	}

	ret = ioctl(fd, FS_IOC_GETFLAGS, &flags);
	if (ret == -1) {
		error("cannot verify file flags/attributes: %m");
		ret = 1;
		goto out;
	}
	if (!(flags & FS_NOCOW_FL)) {
		error("file is not NOCOW");
		ret = 1;
		goto out;
	}
	if (flags & FS_COMPR_FL) {
		error("file with COMP attribute");
		ret = 1;
		goto out;
	}

	ret = read_chunk_tree(fd, &chunks, &num_chunks);
	if (ret == -1)
		goto out;

	ret = map_physical_start(fd, chunks, num_chunks, &physical_start);
	if (ret == 0) {
		if (resume_offset) {
			printf("%llu\n", physical_start / page_size);
		} else {
			pr_verbose(LOG_DEFAULT, "Physical start: %12llu\n",
					physical_start);
			pr_verbose(LOG_DEFAULT, "Resume offset:  %12llu\n",
					physical_start / page_size);
		}
	}

out:
	for (i = 0; i < num_chunks; i++)
		free(chunks[i].stripes);

	free(chunks);
	close(fd);
	return !!ret;
}
static DEFINE_SIMPLE_COMMAND(inspect_map_swapfile, "map-swapfile");

static const char inspect_cmd_group_info[] =
"query various internal information";

static const struct cmd_group inspect_cmd_group = {
	inspect_cmd_group_usage, inspect_cmd_group_info, {
		&cmd_struct_inspect_inode_resolve,
		&cmd_struct_inspect_logical_resolve,
		&cmd_struct_inspect_subvolid_resolve,
		&cmd_struct_inspect_rootid,
		&cmd_struct_inspect_map_swapfile,
		&cmd_struct_inspect_min_dev_size,
		&cmd_struct_inspect_dump_tree,
		&cmd_struct_inspect_dump_super,
		&cmd_struct_inspect_tree_stats,
		&cmd_struct_inspect_list_chunks,
		NULL
	}
};

DEFINE_GROUP_COMMAND(inspect, "inspect-internal");
