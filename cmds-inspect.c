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
#include <sys/ioctl.h>
#include <errno.h>
#include <sys/stat.h>

#include "kerncompat.h"
#include "ioctl.h"
#include "ctree.h"

#include "commands.h"
#include "btrfs-list.h"

static const char * const inspect_cmd_group_usage[] = {
	"btrfs inspect-internal <command> <args>",
	NULL
};

static int __ino_to_path_fd(u64 inum, int fd, int verbose, const char *prepend)
{
	int ret;
	int i;
	struct btrfs_ioctl_ino_path_args ipa;
	struct btrfs_data_container *fspath;

	fspath = malloc(4096);
	if (!fspath)
		return 1;

	ipa.inum = inum;
	ipa.size = 4096;
	ipa.fspath = (u64)fspath;

	ret = ioctl(fd, BTRFS_IOC_INO_PATHS, &ipa);
	if (ret) {
		printf("ioctl ret=%d, error: %s\n", ret, strerror(errno));
		goto out;
	}

	if (verbose)
		printf("ioctl ret=%d, bytes_left=%lu, bytes_missing=%lu, "
			"cnt=%d, missed=%d\n", ret,
			(unsigned long)fspath->bytes_left,
			(unsigned long)fspath->bytes_missing,
			fspath->elem_cnt, fspath->elem_missed);

	for (i = 0; i < fspath->elem_cnt; ++i) {
		char **str = (char **)fspath->val;
		str[i] += (unsigned long)fspath->val;
		if (prepend)
			printf("%s/%s\n", prepend, str[i]);
		else
			printf("%s\n", str[i]);
	}

out:
	free(fspath);
	return ret;
}

static const char * const cmd_inode_resolve_usage[] = {
	"btrfs inspect-internal inode-resolve [-v] <inode> <path>",
	"Get file system paths for the given inode",
	NULL
};

static int cmd_inode_resolve(int argc, char **argv)
{
	int fd;
	int verbose = 0;

	optind = 1;
	while (1) {
		int c = getopt(argc, argv, "v");
		if (c < 0)
			break;

		switch (c) {
		case 'v':
			verbose = 1;
			break;
		default:
			usage(cmd_inode_resolve_usage);
		}
	}

	if (check_argc_exact(argc - optind, 2))
		usage(cmd_inode_resolve_usage);

	fd = open_file_or_dir(argv[optind+1]);
	if (fd < 0) {
		fprintf(stderr, "ERROR: can't access '%s'\n", argv[optind+1]);
		return 12;
	}

	return __ino_to_path_fd(atoll(argv[optind]), fd, verbose,
				argv[optind+1]);
}

static const char * const cmd_logical_resolve_usage[] = {
	"btrfs inspect-internal logical-resolve [-Pv] [-s bufsize] <logical> <path>",
	"Get file system paths for the given logical address",
	"-P          skip the path resolving and print the inodes instead",
	"-v          verbose mode",
	"-s bufsize  set inode container's size. This is used to increase inode",
	"            container's size in case it is not enough to read all the ",
	"            resolved results. The max value one can set is 64k",
	NULL
};

static int cmd_logical_resolve(int argc, char **argv)
{
	int ret;
	int fd;
	int i;
	int verbose = 0;
	int getpath = 1;
	int bytes_left;
	struct btrfs_ioctl_logical_ino_args loi;
	struct btrfs_data_container *inodes;
	u64 size = 4096;
	char full_path[4096];
	char *path_ptr;

	optind = 1;
	while (1) {
		int c = getopt(argc, argv, "Pvs:");
		if (c < 0)
			break;

		switch (c) {
		case 'P':
			getpath = 0;
			break;
		case 'v':
			verbose = 1;
			break;
		case 's':
			size = atoll(optarg);
			break;
		default:
			usage(cmd_logical_resolve_usage);
		}
	}

	if (check_argc_exact(argc - optind, 2))
		usage(cmd_logical_resolve_usage);

	size = min(size, (u64)64 * 1024);
	inodes = malloc(size);
	if (!inodes)
		return 1;

	loi.logical = atoll(argv[optind]);
	loi.size = size;
	loi.inodes = (u64)inodes;

	fd = open_file_or_dir(argv[optind+1]);
	if (fd < 0) {
		fprintf(stderr, "ERROR: can't access '%s'\n", argv[optind+1]);
		ret = 12;
		goto out;
	}

	ret = ioctl(fd, BTRFS_IOC_LOGICAL_INO, &loi);
	if (ret) {
		printf("ioctl ret=%d, error: %s\n", ret, strerror(errno));
		goto out;
	}

	if (verbose)
		printf("ioctl ret=%d, total_size=%llu, bytes_left=%lu, "
			"bytes_missing=%lu, cnt=%d, missed=%d\n",
			ret, size,
			(unsigned long)inodes->bytes_left,
			(unsigned long)inodes->bytes_missing,
			inodes->elem_cnt, inodes->elem_missed);

	bytes_left = sizeof(full_path);
	ret = snprintf(full_path, bytes_left, "%s/", argv[optind+1]);
	path_ptr = full_path + ret;
	bytes_left -= ret + 1;
	BUG_ON(bytes_left < 0);

	for (i = 0; i < inodes->elem_cnt; i += 3) {
		u64 inum = inodes->val[i];
		u64 offset = inodes->val[i+1];
		u64 root = inodes->val[i+2];
		int path_fd;
		char *name;

		if (getpath) {
			name = btrfs_list_path_for_root(fd, root);
			if (IS_ERR(name))
				return PTR_ERR(name);
			if (!name) {
				path_ptr[-1] = '\0';
				path_fd = fd;
			} else {
				path_ptr[-1] = '/';
				ret = snprintf(path_ptr, bytes_left, "%s",
						name);
				BUG_ON(ret >= bytes_left);
				free(name);
				path_fd = open_file_or_dir(full_path);
				if (path_fd < 0) {
					fprintf(stderr, "ERROR: can't access "
						"'%s'\n", full_path);
					goto out;
				}
			}
			__ino_to_path_fd(inum, path_fd, verbose, full_path);
		} else {
			printf("inode %llu offset %llu root %llu\n", inum,
				offset, root);
		}
	}

out:
	free(inodes);
	return ret;
}

static const char * const cmd_bsum_usage[] = {
	"btrfs inspect-internal _bsum <blockno> <file>",
	"Get file block checksum of given file",
	NULL
};

static int csum_for_offset(int fd, u64 offset, u8 *csums, int *count)
{
	struct btrfs_ioctl_search_args args;
	struct btrfs_ioctl_search_key *sk = &args.key;
	struct btrfs_ioctl_search_header *sh;
	int ret = 0;
	int i;
	unsigned off;
	u32 *realcsums = (u32*)csums;

	sk->tree_id = BTRFS_CSUM_TREE_OBJECTID;
	sk->min_objectid = BTRFS_EXTENT_CSUM_OBJECTID;
	sk->max_objectid = BTRFS_EXTENT_CSUM_OBJECTID;
	sk->max_type = BTRFS_EXTENT_CSUM_KEY;
	sk->min_type = BTRFS_EXTENT_CSUM_KEY;
	sk->min_offset = offset;
	sk->max_offset = (u64)-1;
	sk->max_transid = (u64)-1;
	sk->nr_items = 1;

	printf("Search block: %llu\n", (unsigned long long)sk->min_offset);
	ret = ioctl(fd, BTRFS_IOC_TREE_SEARCH, &args);
	if (ret) {
		printf("%s: search ioctl: ioctl ret=%d, error: %s\n", __func__, ret, strerror(errno));
		ret = -1;
		goto out;
	}
	if (sk->nr_items == 0) {
		printf("no items found\n");
		ret = -2;
		goto out;
	}
	off = 0;
	*count = 0;
	for (i = 0; i < sk->nr_items; i++) {
		u32 item;
		int j;
		int csum_size = sizeof(item);

		sh = (struct btrfs_ioctl_search_header *)(args.buf +
				off);

		printf("SH: tid %llu objid %llu off %llu type %u len %u\n",
				(unsigned long long)sh->transid,
				(unsigned long long)sh->objectid,
				(unsigned long long)sh->offset,
				(unsigned)sh->type,
				(unsigned)sh->len);

		off += sizeof(*sh);
		for (j = 0; j < sh->len / csum_size; j++) {
			memcpy(&item, args.buf + off + j * csum_size, sizeof(item));
			printf("DATA[%d]: u32 = 0x%08x\n", j, item);
			realcsums[*count] = item;
			*count += 1;
		}
		off += sh->len;

		sk->min_objectid = sh->objectid;
		sk->min_type = sh->type;
		sk->min_offset = sh->offset;
	}
out:
	return ret;
}

static int extent_offset_to_physical(int fd, u64 ino, u64 offset, u64 *phys)
{
	struct btrfs_ioctl_search_args args;
	struct btrfs_ioctl_search_key *sk = &args.key;
	struct btrfs_ioctl_search_header *sh;
	int ret = 0;
	unsigned off;
	int i;

	sk->tree_id = BTRFS_FS_TREE_OBJECTID;
	sk->min_objectid = ino;
	sk->max_objectid = ino;
	sk->max_type = BTRFS_EXTENT_DATA_KEY;
	sk->min_type = BTRFS_EXTENT_DATA_KEY;
	sk->min_offset = offset;
	sk->max_offset = (u64)-1;
	sk->max_transid = (u64)-1;
	sk->nr_items = 1;

	printf("Search extent offset: %llu\n", (unsigned long long)sk->min_offset);
	ret = ioctl(fd, BTRFS_IOC_TREE_SEARCH, &args);
	if (ret) {
		printf("%s: search ioctl: ioctl ret=%d, error: %s\n", __func__, ret, strerror(errno));
		ret = -1;
		goto out;
	}
	if (sk->nr_items == 0) {
		printf("no items found\n");
		ret = -2;
		goto out;
	}
	off = 0;
	for (i = 0; i < sk->nr_items; i++) {
		struct btrfs_file_extent_item fi;

		sh = (struct btrfs_ioctl_search_header *)(args.buf +
				off);

		printf("SH: tid %llu objid %llu off %llu type %u len %u\n",
				(unsigned long long)sh->transid,
				(unsigned long long)sh->objectid,
				(unsigned long long)sh->offset,
				(unsigned)sh->type,
				(unsigned)sh->len);

		off += sizeof(*sh);
		/* process data */
		memcpy(&fi, args.buf + off, sizeof(fi));
		printf("FI: disk_bytenr %llu disk_num_bytes %llu offset %llu\n",
				(unsigned long long)fi.disk_bytenr,
				(unsigned long long)fi.disk_num_bytes,
				(unsigned long long)fi.offset);
		*phys = fi.disk_bytenr;

		off += sh->len;

		sk->min_objectid = sh->objectid;
		sk->min_type = sh->type;
		sk->min_offset = sh->offset;
	}
out:
	return ret;
}

static int cmd_bsum(int argc, char **argv)
{
	int fd;
	int ret = 0;
	int j;
	struct stat st;
	u8 csums[4096];
	int count = 0;
	u64 offset = 0;
	u64 phys = 0;

	optind = 1;
	while (1) {
		int c = getopt(argc, argv, "");
		if (c < 0)
			break;

		switch (c) {
		default:
			usage(cmd_logical_resolve_usage);
		}
	}

	if (check_argc_exact(argc - optind, 2)) {
		usage(cmd_logical_resolve_usage);
		return 1;
	}

	fd = open_file_or_dir(argv[optind+1]);
	if (fd < 0) {
		fprintf(stderr, "ERROR: can't access '%s'\n", argv[optind+1]);
		ret = 12;
		goto out;
	}
	offset = atoi(argv[optind]);
	if(fstat(fd, &st) == -1) {
		fprintf(stderr, "ERROR: stat\n");
		ret = 1;
		goto out;
	}
	printf("Inode: %llu\n", (unsigned long long)st.st_ino);
	printf("Offset: %llu\n", (unsigned long long)offset);
	extent_offset_to_physical(fd, st.st_ino, offset, &phys);
	printf("Physical: %llu\n", (unsigned long long)phys);
	csum_for_offset(fd, phys, csums, &count);
	for (j = 0; j < count; j++) {
		u32 *items = (u32*)csums;
		printf("BLOCK[%d] CSUM=0x%x\n",
			j, items[j]);
	}

out:
	return ret;
}

const struct cmd_group inspect_cmd_group = {
	inspect_cmd_group_usage, NULL, {
		{ "inode-resolve", cmd_inode_resolve, cmd_inode_resolve_usage,
			NULL, 0 },
		{ "logical-resolve", cmd_logical_resolve,
			cmd_logical_resolve_usage, NULL, 0 },
		{ "_bsum", cmd_bsum,
			cmd_bsum_usage, NULL, 0 },
		{ 0, 0, 0, 0, 0 }
	}
};

int cmd_inspect(int argc, char **argv)
{
	return handle_command_group(&inspect_cmd_group, argc, argv);
}
