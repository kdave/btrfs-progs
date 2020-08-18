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

#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <stdint.h>
#include <sys/ioctl.h>
#include <errno.h>
#include <getopt.h>
#include <limits.h>

#include "kerncompat.h"
#include "ioctl.h"
#include "common/utils.h"
#include "kernel-shared/ctree.h"
#include "common/send-utils.h"
#include "kernel-shared/disk-io.h"
#include "cmds/commands.h"
#include "btrfs-list.h"
#include "common/help.h"

static const char * const inspect_cmd_group_usage[] = {
	"btrfs inspect-internal <command> <args>",
	NULL
};

static int __ino_to_path_fd(u64 inum, int fd, const char *prepend)
{
	int ret;
	int i;
	struct btrfs_ioctl_ino_path_args ipa;
	struct btrfs_data_container fspath[PATH_MAX];

	memset(fspath, 0, sizeof(*fspath));
	ipa.inum = inum;
	ipa.size = PATH_MAX;
	ipa.fspath = ptr_to_u64(fspath);

	ret = ioctl(fd, BTRFS_IOC_INO_PATHS, &ipa);
	if (ret < 0) {
		error("ino paths ioctl: %m");
		goto out;
	}

	pr_verbose(1,
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
			printf("%s/%s\n", prepend, str);
		else
			printf("%s\n", str);
	}

out:
	return !!ret;
}

static const char * const cmd_inspect_inode_resolve_usage[] = {
	"btrfs inspect-internal inode-resolve [-v] <inode> <path>",
	"Get file system paths for the given inode",
	"",
	"-v   deprecated, alias for global -v option",
	HELPINFO_INSERT_GLOBALS,
	HELPINFO_INSERT_VERBOSE,
	NULL
};

static int cmd_inspect_inode_resolve(const struct cmd_struct *cmd,
				     int argc, char **argv)
{
	int fd;
	int ret;
	DIR *dirstream = NULL;

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

	fd = btrfs_open_dir(argv[optind + 1], &dirstream, 1);
	if (fd < 0)
		return 1;

	ret = __ino_to_path_fd(arg_strtou64(argv[optind]), fd, argv[optind+1]);
	close_file_or_dir(fd, dirstream);
	return !!ret;

}
static DEFINE_SIMPLE_COMMAND(inspect_inode_resolve, "inode-resolve");

static const char * const cmd_inspect_logical_resolve_usage[] = {
	"btrfs inspect-internal logical-resolve [-Pvo] [-s bufsize] <logical> <path>",
	"Get file system paths for the given logical address",
	"",
	"-P          skip the path resolving and print the inodes instead",
	"-o          ignore offsets when matching references (requires v2 ioctl",
	"            support in the kernel 4.15+)",
	"-s bufsize  set inode container's size. This is used to increase inode",
	"            container's size in case it is not enough to read all the ",
	"            resolved results. The max value one can set is 64k with the",
	"            v1 ioctl. Sizes over 64k will use the v2 ioctl (kernel 4.15+)",
	"-v          deprecated, alias for global -v option",
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
	int getpath = 1;
	int bytes_left;
	struct btrfs_ioctl_logical_ino_args loi = { 0 };
	struct btrfs_data_container *inodes;
	u64 size = SZ_64K;
	char full_path[PATH_MAX];
	char *path_ptr;
	DIR *dirstream = NULL;
	u64 flags = 0;
	unsigned long request = BTRFS_IOC_LOGICAL_INO;

	optind = 0;
	while (1) {
		int c = getopt(argc, argv, "Pvos:");
		if (c < 0)
			break;

		switch (c) {
		case 'P':
			getpath = 0;
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

	fd = btrfs_open_dir(argv[optind + 1], &dirstream, 1);
	if (fd < 0) {
		ret = 12;
		goto out;
	}

	ret = ioctl(fd, request, &loi);
	if (ret < 0) {
		error("logical ino ioctl: %m");
		goto out;
	}

	pr_verbose(1,
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
		char *name;
		DIR *dirs = NULL;

		if (getpath) {
			name = btrfs_list_path_for_root(fd, root);
			if (IS_ERR(name)) {
				ret = PTR_ERR(name);
				goto out;
			}
			if (!name) {
				path_ptr[-1] = '\0';
				path_fd = fd;
			} else {
				path_ptr[-1] = '/';
				ret = snprintf(path_ptr, bytes_left, "%s",
						name);
				free(name);
				if (ret >= bytes_left) {
					error("path buffer too small: %d bytes",
							bytes_left - ret);
					goto out;
				}
				path_fd = btrfs_open_dir(full_path, &dirs, 1);
				if (path_fd < 0) {
					ret = -ENOENT;
					goto out;
				}
			}
			ret = __ino_to_path_fd(inum, path_fd, full_path);
			if (path_fd != fd)
				close_file_or_dir(path_fd, dirs);
		} else {
			printf("inode %llu offset %llu root %llu\n", inum,
				offset, root);
		}
	}

out:
	close_file_or_dir(fd, dirstream);
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
	DIR *dirstream = NULL;

	clean_args_no_options(cmd, argc, argv);

	if (check_argc_exact(argc - optind, 2))
		return 1;

	fd = btrfs_open_dir(argv[optind + 1], &dirstream, 1);
	if (fd < 0) {
		ret = -ENOENT;
		goto out;
	}

	subvol_id = arg_strtou64(argv[optind]);
	ret = btrfs_subvolid_resolve(fd, path, sizeof(path), subvol_id);

	if (ret) {
		error("resolving subvolid %llu error %d",
			(unsigned long long)subvol_id, ret);
		goto out;
	}

	path[PATH_MAX - 1] = '\0';
	printf("%s\n", path);

out:
	close_file_or_dir(fd, dirstream);
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
	DIR *dirstream = NULL;

	clean_args_no_options(cmd, argc, argv);

	if (check_argc_exact(argc - optind, 1))
		return 1;

	fd = btrfs_open_file_or_dir(argv[optind], &dirstream, 1);
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

	printf("%llu\n", (unsigned long long)rootid);
out:
	close_file_or_dir(fd, dirstream);

	return !!ret;
}
static DEFINE_SIMPLE_COMMAND(inspect_rootid, "rootid");

static const char* const cmd_inspect_min_dev_size_usage[] = {
	"btrfs inspect-internal min-dev-size [options] <path>",
	"Get the minimum size the device can be shrunk to. The",
	"device id 1 is used by default.",
	"",
	"--id DEVID   specify the device id to query",
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
	struct btrfs_ioctl_search_args args;
	struct btrfs_ioctl_search_key *sk = &args.key;
	u64 last_pos = (u64)-1;
	LIST_HEAD(extents);
	LIST_HEAD(holes);

	memset(&args, 0, sizeof(args));
	sk->tree_id = BTRFS_DEV_TREE_OBJECTID;
	sk->min_objectid = devid;
	sk->max_objectid = devid;
	sk->max_type = BTRFS_DEV_EXTENT_KEY;
	sk->min_type = BTRFS_DEV_EXTENT_KEY;
	sk->min_offset = 0;
	sk->max_offset = (u64)-1;
	sk->min_transid = 0;
	sk->max_transid = (u64)-1;
	sk->nr_items = 4096;

	while (1) {
		int i;
		struct btrfs_ioctl_search_header *sh;
		unsigned long off = 0;

		ret = ioctl(fd, BTRFS_IOC_TREE_SEARCH, &args);
		if (ret < 0) {
			error("tree search ioctl: %m");
			ret = 1;
			goto out;
		}

		if (sk->nr_items == 0)
			break;

		for (i = 0; i < sk->nr_items; i++) {
			struct btrfs_dev_extent *extent;
			u64 len;

			sh = (struct btrfs_ioctl_search_header *)(args.buf +
								  off);
			off += sizeof(*sh);
			extent = (struct btrfs_dev_extent *)(args.buf + off);
			off += btrfs_search_header_len(sh);

			sk->min_objectid = btrfs_search_header_objectid(sh);
			sk->min_type = btrfs_search_header_type(sh);
			sk->min_offset = btrfs_search_header_offset(sh) + 1;

			if (btrfs_search_header_objectid(sh) != devid ||
			    btrfs_search_header_type(sh) != BTRFS_DEV_EXTENT_KEY)
				continue;

			len = btrfs_stack_dev_extent_length(extent);
			min_size += len;
			ret = add_dev_extent(&extents,
				btrfs_search_header_offset(sh),
				btrfs_search_header_offset(sh) + len - 1, 0);

			if (!ret && last_pos != (u64)-1 &&
			    last_pos != btrfs_search_header_offset(sh))
				ret = add_dev_extent(&holes, last_pos,
					btrfs_search_header_offset(sh) - 1, 1);
			if (ret) {
				errno = -ret;
				error("add device extent: %m");
				ret = 1;
				goto out;
			}

			last_pos = btrfs_search_header_offset(sh) + len;
		}

		if (sk->min_type != BTRFS_DEV_EXTENT_KEY ||
		    sk->min_objectid != devid)
			break;
	}

	adjust_dev_min_size(&extents, &holes, &min_size);
	printf("%llu bytes (%s)\n", min_size, pretty_size(min_size));
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
	DIR *dirstream = NULL;
	u64 devid = 1;

	optind = 0;
	while (1) {
		int c;
		enum { GETOPT_VAL_DEVID = 256 };
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

	fd = btrfs_open_dir(argv[optind], &dirstream, 1);
	if (fd < 0) {
		ret = -ENOENT;
		goto out;
	}

	ret = print_min_dev_size(fd, devid);
	close_file_or_dir(fd, dirstream);
out:
	return !!ret;
}
static DEFINE_SIMPLE_COMMAND(inspect_min_dev_size, "min-dev-size");

static const char inspect_cmd_group_info[] =
"query various internal information";

static const struct cmd_group inspect_cmd_group = {
	inspect_cmd_group_usage, inspect_cmd_group_info, {
		&cmd_struct_inspect_inode_resolve,
		&cmd_struct_inspect_logical_resolve,
		&cmd_struct_inspect_subvolid_resolve,
		&cmd_struct_inspect_rootid,
		&cmd_struct_inspect_min_dev_size,
		&cmd_struct_inspect_dump_tree,
		&cmd_struct_inspect_dump_super,
		&cmd_struct_inspect_tree_stats,
		NULL
	}
};

DEFINE_GROUP_COMMAND(inspect, "inspect-internal");
