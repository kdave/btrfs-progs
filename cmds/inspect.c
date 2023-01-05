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
#include <sys/fcntl.h>
#include <sys/stat.h>
#include <sys/statfs.h>
#include <linux/fs.h>
#include <linux/magic.h>
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
#include "cmds/commands.h"
#include "ioctl.h"

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
	bool getpath = true;
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
		DIR *dirs = NULL;

		if (getpath) {
			char mount_path[PATH_MAX];
			char name[PATH_MAX];

			ret = btrfs_subvolid_resolve(fd, name, sizeof(name), root);
			if (ret < 0)
				goto out;

			if (name[0] == 0) {
				path_ptr[-1] = '\0';
				path_fd = fd;
				strncpy(mount_path, full_path, PATH_MAX);
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

				strncpy(mount_path, mounted, PATH_MAX);
				free(mounted);

				path_fd = btrfs_open_dir(mount_path, &dirs, 1);
				if (path_fd < 0) {
					ret = -ENOENT;
					goto out;
				}
			}
			ret = __ino_to_path_fd(inum, path_fd, mount_path);
			if (path_fd != fd)
				close_file_or_dir(path_fd, dirs);
		} else {
			pr_verbose(LOG_DEFAULT, "inode %llu offset %llu root %llu\n", inum,
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
		error("resolving subvolid %llu error %d", subvol_id, ret);
		goto out;
	}

	path[PATH_MAX - 1] = '\0';
	pr_verbose(LOG_DEFAULT, "%s\n", path);

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

	pr_verbose(LOG_DEFAULT, "%llu\n", rootid);
out:
	close_file_or_dir(fd, dirstream);

	return !!ret;
}
static DEFINE_SIMPLE_COMMAND(inspect_rootid, "rootid");

static const char* const cmd_inspect_min_dev_size_usage[] = {
	"btrfs inspect-internal min-dev-size [options] <path>",
	"Get the minimum size the device can be shrunk to",
	"",
	"The device id 1 is used by default.",
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
	DIR *dirstream = NULL;
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

#if EXPERIMENTAL

static const char * const cmd_inspect_list_chunks_usage[] = {
	"btrfs inspect-internal list-chunks [options] <path>",
	"Show chunks (block groups) layout",
	"Show chunks (block groups) layout for all devices",
	"",
	HELPINFO_UNITS_LONG,
	"--sort=MODE        sort by the physical or logical chunk start",
	"                   MODE is one of pstart or lstart (default: pstart)",
	"--usage            show usage per block group (note: this can be slow)",
	"--no-usage         don't show usage per block group",
	"--empty            show empty space between block groups",
	"--no-empty         do not show empty space between block groups",
	NULL
};

enum {
	CHUNK_SORT_PSTART,
	CHUNK_SORT_LSTART,
	CHUNK_SORT_DEFAULT = CHUNK_SORT_PSTART
};

struct list_chunks_entry {
	u64 devid;
	u64 start;
	u64 lstart;
	u64 length;
	u64 flags;
	u64 age;
	u64 used;
	u32 pnumber;
};

struct list_chunks_ctx {
	unsigned length;
	unsigned size;
	struct list_chunks_entry *stats;
};

int cmp_cse_devid_start(const void *va, const void *vb)
{
	const struct list_chunks_entry *a = va;
	const struct list_chunks_entry *b = vb;

	if (a->devid < b->devid)
		return -1;
	if (a->devid > b->devid)
		return 1;

	if (a->start < b->start)
		return -1;
	if (a->start == b->start) {
		error(
	"chunks start on same offset in the same device: devid %llu start %llu",
		    (unsigned long long)a->devid, (unsigned long long)a->start);
		return 0;
	}
	return 1;
}

int cmp_cse_devid_lstart(const void *va, const void *vb)
{
	const struct list_chunks_entry *a = va;
	const struct list_chunks_entry *b = vb;

	if (a->devid < b->devid)
		return -1;
	if (a->devid > b->devid)
		return 1;

	if (a->lstart < b->lstart)
		return -1;
	if (a->lstart == b->lstart) {
		error(
"chunks logically start on same offset in the same device: devid %llu start %llu",
		    (unsigned long long)a->devid, (unsigned long long)a->lstart);
		return 0;
	}
	return 1;
}

int print_list_chunks(struct list_chunks_ctx *ctx, unsigned sort_mode,
		      unsigned unit_mode, bool with_usage, bool with_empty)
{
	u64 devid;
	struct list_chunks_entry e;
	struct string_table *table;
	int i;
	int chidx;
	u64 lastend;
	u64 age;
	u32 gaps;
	u32 tabidx;

	/*
	 * Chunks are sorted logically as found by the ioctl, we need to sort
	 * them once to find the physical ordering. This is the default mode.
	 */
	qsort(ctx->stats, ctx->length, sizeof(ctx->stats[0]), cmp_cse_devid_start);
	devid = 0;
	age = 0;
	gaps = 0;
	lastend = 0;
	for (i = 0; i < ctx->length; i++) {
		e = ctx->stats[i];
		if (e.devid != devid) {
			devid = e.devid;
			age = 0;
		}
		ctx->stats[i].pnumber = age;
		age++;
		if (with_empty && sort_mode == CHUNK_SORT_PSTART && e.start != lastend)
			gaps++;
		lastend = e.start + e.length;
	}

	if (sort_mode == CHUNK_SORT_LSTART)
		qsort(ctx->stats, ctx->length, sizeof(ctx->stats[0]),
				cmp_cse_devid_lstart);

	/* Optional usage, two rows for header and separator, gaps */
	table = table_create(7 + (int)with_usage, 2 + ctx->length + gaps);
	if (!table) {
		error_msg(ERROR_MSG_MEMORY, NULL);
		return 1;
	}
	devid = 0;
	tabidx = 0;
	chidx = 1;
	for (i = 0; i < ctx->length; i++) {
		e = ctx->stats[i];
		/* TODO: print header and devid */
		if (e.devid != devid) {
			int j;

			devid = e.devid;
			table_printf(table, 0, tabidx, ">PNumber");
			table_printf(table, 1, tabidx, ">Type");
			table_printf(table, 2, tabidx, ">PStart");
			table_printf(table, 3, tabidx, ">Length");
			table_printf(table, 4, tabidx, ">PEnd");
			table_printf(table, 5, tabidx, ">Age");
			table_printf(table, 6, tabidx, ">LStart");
			if (with_usage) {
				table_printf(table, 7, tabidx, ">Usage%%");
				table_printf(table, 7, tabidx + 1, "*-");
			}
			for (j = 0; j < 7; j++)
				table_printf(table, j, tabidx + 1, "*-");

			chidx = 1;
			lastend = 0;
			tabidx += 2;
		}
		if (with_empty && sort_mode == CHUNK_SORT_PSTART && e.start != lastend) {
			table_printf(table, 0, tabidx, ">-");
			table_printf(table, 1, tabidx, ">%s", "empty");
			table_printf(table, 2, tabidx, ">-");
			table_printf(table, 3, tabidx, ">%s",
				pretty_size_mode(e.start - lastend, unit_mode));
			table_printf(table, 4, tabidx, ">-");
			table_printf(table, 5, tabidx, ">-");
			table_printf(table, 6, tabidx, ">-");
			if (with_usage)
				table_printf(table, 7, tabidx, ">-");
			tabidx++;
		}

		table_printf(table, 0, tabidx, ">%llu", chidx++);
		table_printf(table, 1, tabidx, ">%10s/%-6s",
				btrfs_group_type_str(e.flags),
				btrfs_group_profile_str(e.flags));
		table_printf(table, 2, tabidx, ">%s", pretty_size_mode(e.start, unit_mode));
		table_printf(table, 3, tabidx, ">%s", pretty_size_mode(e.length, unit_mode));
		table_printf(table, 4, tabidx, ">%s", pretty_size_mode(e.start + e.length, unit_mode));
		table_printf(table, 5, tabidx, ">%llu", e.age);
		table_printf(table, 6, tabidx, ">%s", pretty_size_mode(e.lstart, unit_mode));
		if (with_usage)
			table_printf(table, 7, tabidx, ">%6.2f",
					(float)e.used / e.length * 100);
		lastend = e.start + e.length;
		tabidx++;
	}
	table_dump(table);
	table_free(table);

	return 0;
}

static u64 fill_usage(int fd, u64 lstart)
{
	struct btrfs_ioctl_search_args args;
	struct btrfs_ioctl_search_key *sk = &args.key;
	struct btrfs_ioctl_search_header sh;
	struct btrfs_block_group_item *item;
	int ret;

	memset(&args, 0, sizeof(args));
	sk->tree_id = BTRFS_EXTENT_TREE_OBJECTID;
	sk->min_objectid = lstart;
	sk->max_objectid = lstart;
	sk->min_type = BTRFS_BLOCK_GROUP_ITEM_KEY;
	sk->max_type = BTRFS_BLOCK_GROUP_ITEM_KEY;
	sk->min_offset = 0;
	sk->max_offset = (u64)-1;
	sk->max_transid = (u64)-1;
	sk->nr_items = 1;

	ret = ioctl(fd, BTRFS_IOC_TREE_SEARCH, &args);
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

	memcpy(&sh, args.buf, sizeof(sh));
	item = (struct btrfs_block_group_item*)(args.buf + sizeof(sh));

	return item->used;
}

static int cmd_inspect_list_chunks(const struct cmd_struct *cmd,
				   int argc, char **argv)
{
	struct btrfs_ioctl_search_args args;
	struct btrfs_ioctl_search_key *sk = &args.key;
	struct btrfs_ioctl_search_header sh;
	unsigned long off = 0;
	u64 *age = NULL;
	unsigned age_size = 128;
	int ret;
	int fd;
	int i;
	int e;
	DIR *dirstream = NULL;
	unsigned unit_mode;
	unsigned sort_mode = 0;
	bool with_usage = true;
	bool with_empty = true;
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
		       GETOPT_VAL_USAGE, GETOPT_VAL_NO_USAGE,
		       GETOPT_VAL_EMPTY, GETOPT_VAL_NO_EMPTY
		};
		static const struct option long_options[] = {
			{"sort", required_argument, NULL, GETOPT_VAL_SORT },
			{"usage", no_argument, NULL, GETOPT_VAL_USAGE },
			{"no-usage", no_argument, NULL, GETOPT_VAL_NO_USAGE },
			{"empty", no_argument, NULL, GETOPT_VAL_EMPTY },
			{"no-empty", no_argument, NULL, GETOPT_VAL_NO_EMPTY },
			{NULL, 0, NULL, 0}
		};

		c = getopt_long(argc, argv, "", long_options, NULL);
		if (c < 0)
			break;

		switch (c) {
		case GETOPT_VAL_SORT:
			if (strcmp(optarg, "pstart") == 0) {
				sort_mode = CHUNK_SORT_PSTART;
			} else if (strcmp(optarg, "lstart") == 0) {
				sort_mode = CHUNK_SORT_LSTART;
			} else {
				error("unknown sort mode: %s", optarg);
				exit(1);
			}
			break;
		case GETOPT_VAL_USAGE:
		case GETOPT_VAL_NO_USAGE:
			with_usage = (c == GETOPT_VAL_USAGE);
			break;
		case GETOPT_VAL_EMPTY:
		case GETOPT_VAL_NO_EMPTY:
			with_empty = (c == GETOPT_VAL_EMPTY);
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
		error_msg(ERROR_MSG_MEMORY, NULL);
		goto out_nomem;
	}

	path = argv[optind];

	fd = open_file_or_dir(path, &dirstream);
	if (fd < 0) {
	        error("cannot access '%s': %m", path);
		return 1;
	}

	memset(&args, 0, sizeof(args));
	sk->tree_id = BTRFS_CHUNK_TREE_OBJECTID;
	sk->min_objectid = BTRFS_FIRST_CHUNK_TREE_OBJECTID;
	sk->max_objectid = BTRFS_FIRST_CHUNK_TREE_OBJECTID;
	sk->min_type = BTRFS_CHUNK_ITEM_KEY;
	sk->max_type = BTRFS_CHUNK_ITEM_KEY;
	sk->max_offset = (u64)-1;
	sk->max_transid = (u64)-1;
	age = calloc(age_size, sizeof(u64));
	if (!age) {
		ret = 1;
		error_msg(ERROR_MSG_MEMORY, NULL);
		goto out_nomem;
	}

	while (1) {
		sk->nr_items = 1;
		ret = ioctl(fd, BTRFS_IOC_TREE_SEARCH, &args);
		e = errno;
		if (ret < 0) {
			error("cannot perform the search: %s", strerror(e));
			return 1;
		}
		if (sk->nr_items == 0)
			break;

		off = 0;
		for (i = 0; i < sk->nr_items; i++) {
			struct btrfs_chunk *item;
			struct btrfs_stripe *stripes;
			int sidx;
			u64 used = (u64)-1;

			memcpy(&sh, args.buf + off, sizeof(sh));
			off += sizeof(sh);
			item = (struct btrfs_chunk*)(args.buf + off);
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
				while (devid > age_size) {
					u64 *tmp;
					unsigned old_size = age_size;

					age_size += 128;
					tmp = calloc(age_size, sizeof(u64));
					if (!tmp) {
						ret = 1;
						error_msg(ERROR_MSG_MEMORY, NULL);
						goto out_nomem;
					}
					memcpy(tmp, age, sizeof(u64) * old_size);
					age = tmp;
				}
				e->age = age[devid]++;
				if (with_usage) {
					if (used == (u64)-1)
						used = fill_usage(fd, sh.offset);
					e->used = used;
				} else {
					e->used = 0;
				}

				ctx.length++;

				if (ctx.length == ctx.size) {
					ctx.size += 1024;
					ctx.stats = realloc(ctx.stats, ctx.size
						* sizeof(ctx.stats[0]));
					if (!ctx.stats) {
						ret = 1;
						error_msg(ERROR_MSG_MEMORY, NULL);
						goto out_nomem;
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

	ret = print_list_chunks(&ctx, sort_mode, unit_mode, with_usage, with_empty);
	close_file_or_dir(fd, dirstream);

out_nomem:
	free(ctx.stats);
	free(age);

	return !!ret;
}
static DEFINE_SIMPLE_COMMAND(inspect_list_chunks, "list-chunks");

#endif

static const char * const cmd_inspect_map_swapfile_usage[] = {
	"btrfs inspect-internal map-swapfile <file>",
	"Print physical offset of first block and resume offset if file is suitable as swapfile",
	"Print physical offset of first block and resume offset if file is suitable as swapfile.",
	"All conditions of a swapfile extents are verified if they could pass kernel tests.",
	"Use the value of resume offset for /sys/power/resume_offset, this depends on the",
	"page size that's detected on this system.",
	"",
	"-r|--resume-offset   print only the value of resume_offset",
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
	struct btrfs_ioctl_search_args search = {
		.key = {
			.tree_id = BTRFS_CHUNK_TREE_OBJECTID,
			.min_objectid = BTRFS_FIRST_CHUNK_TREE_OBJECTID,
			.min_type = BTRFS_CHUNK_ITEM_KEY,
			.min_offset = 0,
			.max_objectid = BTRFS_FIRST_CHUNK_TREE_OBJECTID,
			.max_type = BTRFS_CHUNK_ITEM_KEY,
			.max_offset = (u64)-1,
			.min_transid = 0,
			.max_transid = (u64)-1,
			.nr_items = 0,
		},
	};
	size_t items_pos = 0, buf_off = 0;
	size_t capacity = 0;
	int ret;

	*chunks = NULL;
	*num_chunks = 0;
	for (;;) {
		const struct btrfs_ioctl_search_header *header;
		const struct btrfs_chunk *item;
		struct chunk *chunk;
		size_t i;

		if (items_pos >= search.key.nr_items) {
			search.key.nr_items = 4096;
			ret = ioctl(fd, BTRFS_IOC_TREE_SEARCH, &search);
			if (ret == -1) {
				perror("BTRFS_IOC_TREE_SEARCH");
				return -1;
			}
			items_pos = 0;
			buf_off = 0;

			if (search.key.nr_items == 0)
				break;
		}

		header = (struct btrfs_ioctl_search_header *)(search.buf + buf_off);
		if (header->type != BTRFS_CHUNK_ITEM_KEY)
			goto next;

		item = (void *)(header + 1);
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
		chunk->offset = header->offset;
		chunk->length = le64_to_cpu(item->length);
		chunk->stripe_len = le64_to_cpu(item->stripe_len);
		chunk->type = le64_to_cpu(item->type);
		chunk->num_stripes = le16_to_cpu(item->num_stripes);
		chunk->sub_stripes = le16_to_cpu(item->sub_stripes);
		chunk->stripes = calloc(chunk->num_stripes,
					sizeof(*chunk->stripes));
		if (!chunk->stripes) {
			perror("calloc");
			return -1;
		}
		(*num_chunks)++;

		for (i = 0; i < chunk->num_stripes; i++) {
			const struct btrfs_stripe *stripe;

			stripe = &item->stripe + i;
			chunk->stripes[i].devid = le64_to_cpu(stripe->devid);
			chunk->stripes[i].offset = le64_to_cpu(stripe->offset);
		}

next:
		items_pos++;
		buf_off += sizeof(*header) + header->len;
		if (header->offset == (u64)-1)
			break;
		else
			search.key.min_offset = header->offset + 1;
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
	struct btrfs_ioctl_search_args search = {
		.key = {
			.min_type = BTRFS_EXTENT_DATA_KEY,
			.max_type = BTRFS_EXTENT_DATA_KEY,
			.min_offset = 0,
			.max_offset = (u64)-1,
			.min_transid = 0,
			.max_transid = (u64)-1,
			.nr_items = 0,
		},
	};
	struct btrfs_ioctl_ino_lookup_args args = {
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

	ret = ioctl(fd, BTRFS_IOC_INO_LOOKUP, &args);
	if (ret == -1) {
		error("cannot lookup parent subvolume: %m");
		return -errno;
	}

	search.key.tree_id = args.treeid;
	search.key.min_objectid = search.key.max_objectid = st.st_ino;
	for (;;) {
		const struct btrfs_ioctl_search_header *header;
		const struct btrfs_file_extent_item *item;
		u8 type;
		u64 logical_offset = 0;
		struct chunk *chunk = NULL;

		if (items_pos >= search.key.nr_items) {
			search.key.nr_items = 4096;
			ret = ioctl(fd, BTRFS_IOC_TREE_SEARCH, &search);
			if (ret == -1) {
				error("cannot search tree: %m");
				return -errno;
			}
			items_pos = 0;
			buf_off = 0;

			if (search.key.nr_items == 0)
				break;
		}

		header = (struct btrfs_ioctl_search_header *)(search.buf + buf_off);
		if (header->type != BTRFS_EXTENT_DATA_KEY)
			goto next;

		item = (void *)(header + 1);

		type = item->type;
		if (type == BTRFS_FILE_EXTENT_REG ||
		    type == BTRFS_FILE_EXTENT_PREALLOC) {
			logical_offset = le64_to_cpu(item->disk_bytenr);
			if (logical_offset) {
				/* Regular extent */
				chunk = find_chunk(chunks, num_chunks, logical_offset);
				if (!chunk) {
					error("cannot find chunk containing %llu",
						(unsigned long long)logical_offset);
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
			error("file with other_encoding: %u", le16_to_cpu(item->other_encoding));
			ret = -EINVAL;
			goto out;
		}

		/* Only single profile */
		if ((chunk->type & BTRFS_BLOCK_GROUP_PROFILE_MASK) != 0) {
			error("unsupported block group profile: %llu",
				(unsigned long long)(chunk->type & BTRFS_BLOCK_GROUP_PROFILE_MASK));
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
		buf_off += sizeof(*header) + header->len;
		if (header->offset == (u64)-1)
			break;
		else
			search.key.min_offset = header->offset + 1;
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
#if EXPERIMENTAL
		&cmd_struct_inspect_list_chunks,
#endif
		NULL
	}
};

DEFINE_GROUP_COMMAND(inspect, "inspect-internal");
