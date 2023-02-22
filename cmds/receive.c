/*
 * Copyright (C) 2012 Alexander Block.  All rights reserved.
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

#include "kerncompat.h"
#include <sys/stat.h>
#include <sys/ioctl.h>
#include <sys/xattr.h>
#include <linux/fs.h>
#if HAVE_LINUX_FSVERITY_H
#include <linux/fsverity.h>
#endif
#include <unistd.h>
#include <stdint.h>
#include <fcntl.h>
#include <getopt.h>
#include <limits.h>
#include <errno.h>
#include <endian.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <uuid/uuid.h>
#include <zlib.h>
#if COMPRESSION_LZO
#include <lzo/lzoconf.h>
#include <lzo/lzo1x.h>
#endif
#if COMPRESSION_ZSTD
#include <zstd.h>
#endif
#include "kernel-shared/ctree.h"
#include "common/defs.h"
#include "common/messages.h"
#include "common/utils.h"
#include "common/send-stream.h"
#include "common/send-utils.h"
#include "common/help.h"
#include "common/path-utils.h"
#include "common/string-utils.h"
#include "cmds/commands.h"
#include "cmds/receive-dump.h"
#include "ioctl.h"

struct btrfs_receive
{
	int mnt_fd;
	int dest_dir_fd;

	int write_fd;
	char write_path[PATH_MAX];

	char *root_path;
	char *dest_dir_path; /* relative to root_path */
	char full_subvol_path[PATH_MAX];
	char *full_root_path;

	struct subvol_info cur_subvol;
	/*
	 * Substitute for cur_subvol::path which is a pointer and we cannot
	 * change it to an array as it's a public API.
	 */
	char cur_subvol_path[PATH_MAX];

	bool dest_dir_chroot;

	bool honor_end_cmd;

	bool force_decompress;

#if COMPRESSION_ZSTD
	/* Reuse stream objects for encoded_write decompression fallback */
	ZSTD_DStream *zstd_dstream;
#endif
	z_stream *zlib_stream;
};

static int finish_subvol(struct btrfs_receive *rctx)
{
	int ret;
	int subvol_fd = -1;
	struct btrfs_ioctl_received_subvol_args rs_args;
	char uuid_str[BTRFS_UUID_UNPARSED_SIZE];
	u64 flags;

	if (rctx->cur_subvol_path[0] == 0)
		return 0;

	subvol_fd = openat(rctx->mnt_fd, rctx->cur_subvol_path,
			   O_RDONLY | O_NOATIME);
	if (subvol_fd < 0) {
		ret = -errno;
		error("cannot open %s: %m", rctx->cur_subvol_path);
		goto out;
	}

	memset(&rs_args, 0, sizeof(rs_args));
	memcpy(rs_args.uuid, rctx->cur_subvol.received_uuid, BTRFS_UUID_SIZE);
	rs_args.stransid = rctx->cur_subvol.stransid;

	if (bconf.verbose >= 2) {
		uuid_unparse((u8*)rs_args.uuid, uuid_str);
		fprintf(stderr, "BTRFS_IOC_SET_RECEIVED_SUBVOL uuid=%s, "
				"stransid=%llu\n", uuid_str, rs_args.stransid);
	}

	ret = ioctl(subvol_fd, BTRFS_IOC_SET_RECEIVED_SUBVOL, &rs_args);
	if (ret < 0) {
		ret = -errno;
		error("ioctl BTRFS_IOC_SET_RECEIVED_SUBVOL failed: %m");
		goto out;
	}
	rctx->cur_subvol.rtransid = rs_args.rtransid;

	ret = ioctl(subvol_fd, BTRFS_IOC_SUBVOL_GETFLAGS, &flags);
	if (ret < 0) {
		ret = -errno;
		error("ioctl BTRFS_IOC_SUBVOL_GETFLAGS failed: %m");
		goto out;
	}

	flags |= BTRFS_SUBVOL_RDONLY;

	ret = ioctl(subvol_fd, BTRFS_IOC_SUBVOL_SETFLAGS, &flags);
	if (ret < 0) {
		ret = -errno;
		error("failed to make subvolume read only: %m");
		goto out;
	}

	ret = 0;

out:
	if (rctx->cur_subvol_path[0]) {
		rctx->cur_subvol_path[0] = 0;
	}
	if (subvol_fd != -1)
		close(subvol_fd);
	return ret;
}

static int process_subvol(const char *path, const u8 *uuid, u64 ctransid,
			  void *user)
{
	int ret;
	struct btrfs_receive *rctx = user;
	struct btrfs_ioctl_vol_args args_v1;
	char uuid_str[BTRFS_UUID_UNPARSED_SIZE];

	ret = finish_subvol(rctx);
	if (ret < 0)
		goto out;

	if (rctx->cur_subvol.path) {
		error("subvol: another one already started, path ptr: %s",
				rctx->cur_subvol.path);
		ret = -EINVAL;
		goto out;
	}
	if (rctx->cur_subvol_path[0]) {
		error("subvol: another one already started, path buf: %s",
				rctx->cur_subvol_path);
		ret = -EINVAL;
		goto out;
	}

	if (*rctx->dest_dir_path == 0) {
		strncpy_null(rctx->cur_subvol_path, path);
	} else {
		ret = path_cat_out(rctx->cur_subvol_path, rctx->dest_dir_path,
				   path);
		if (ret < 0) {
			error("subvol: path invalid: %s", path);
			goto out;
		}
	}
	ret = path_cat3_out(rctx->full_subvol_path, rctx->root_path,
			    rctx->dest_dir_path, path);
	if (ret < 0) {
		error("subvol: path invalid: %s", path);
		goto out;
	}

	if (bconf.verbose > BTRFS_BCONF_QUIET)
		fprintf(stderr, "At subvol %s\n", path);

	memcpy(rctx->cur_subvol.received_uuid, uuid, BTRFS_UUID_SIZE);
	rctx->cur_subvol.stransid = ctransid;

	if (bconf.verbose >= 2) {
		uuid_unparse((u8*)rctx->cur_subvol.received_uuid, uuid_str);
		fprintf(stderr, "receiving subvol %s uuid=%s, stransid=%llu\n",
				path, uuid_str,
				rctx->cur_subvol.stransid);
	}

	memset(&args_v1, 0, sizeof(args_v1));
	strncpy_null(args_v1.name, path);
	ret = ioctl(rctx->dest_dir_fd, BTRFS_IOC_SUBVOL_CREATE, &args_v1);
	if (ret < 0) {
		ret = -errno;
		error("creating subvolume %s failed: %m", path);
		goto out;
	}

out:
	return ret;
}

static int process_snapshot(const char *path, const u8 *uuid, u64 ctransid,
			    const u8 *parent_uuid, u64 parent_ctransid,
			    void *user)
{
	int ret;
	struct btrfs_receive *rctx = user;
	char uuid_str[BTRFS_UUID_UNPARSED_SIZE];
	struct btrfs_ioctl_vol_args_v2 args_v2;
	struct subvol_info *parent_subvol = NULL;

	ret = finish_subvol(rctx);
	if (ret < 0)
		goto out;

	if (rctx->cur_subvol.path) {
		error("snapshot: another one already started, path ptr: %s",
				rctx->cur_subvol.path);
		ret = -EINVAL;
		goto out;
	}
	if (rctx->cur_subvol_path[0]) {
		error("snapshot: another one already started, path buf: %s",
				rctx->cur_subvol_path);
		ret = -EINVAL;
		goto out;
	}

	if (*rctx->dest_dir_path == 0) {
		strncpy_null(rctx->cur_subvol_path, path);
	} else {
		ret = path_cat_out(rctx->cur_subvol_path, rctx->dest_dir_path,
				   path);
		if (ret < 0) {
			error("snapshot: path invalid: %s", path);
			goto out;
		}
	}
	ret = path_cat3_out(rctx->full_subvol_path, rctx->root_path,
			    rctx->dest_dir_path, path);
	if (ret < 0) {
		error("snapshot: path invalid: %s", path);
		goto out;
	}

	pr_verbose(1, "At snapshot %s\n", path);

	memcpy(rctx->cur_subvol.received_uuid, uuid, BTRFS_UUID_SIZE);
	rctx->cur_subvol.stransid = ctransid;

	if (bconf.verbose >= 2) {
		uuid_unparse((u8*)rctx->cur_subvol.received_uuid, uuid_str);
		fprintf(stderr, "receiving snapshot %s uuid=%s, "
				"ctransid=%llu ", path, uuid_str,
				rctx->cur_subvol.stransid);
		uuid_unparse(parent_uuid, uuid_str);
		fprintf(stderr, "parent_uuid=%s, parent_ctransid=%llu\n",
				uuid_str, parent_ctransid);
	}

	memset(&args_v2, 0, sizeof(args_v2));
	strncpy_null(args_v2.name, path);

	parent_subvol = subvol_uuid_search(rctx->mnt_fd, 0, parent_uuid,
					   parent_ctransid, NULL,
					   subvol_search_by_received_uuid);
	if (IS_ERR_OR_NULL(parent_subvol)) {
		parent_subvol = subvol_uuid_search(rctx->mnt_fd, 0, parent_uuid,
						   parent_ctransid, NULL,
						   subvol_search_by_uuid);
	}
	if (IS_ERR_OR_NULL(parent_subvol)) {
		if (!parent_subvol)
			ret = -ENOENT;
		else
			ret = PTR_ERR(parent_subvol);
		error("cannot find parent subvolume");
		goto out;
	}

	/*
	 * The path is resolved from the root subvol, but we could be in some
	 * subvolume under the root subvolume, so try and adjust the path to be
	 * relative to our root path.
	 */
	if (rctx->full_root_path) {
		size_t root_len;
		size_t sub_len;

		root_len = strlen(rctx->full_root_path);
		sub_len = strlen(parent_subvol->path);

		/* First make sure the parent subvol is actually in our path */
		if (strstr(parent_subvol->path, rctx->full_root_path) != parent_subvol->path ||
		    (sub_len > root_len && parent_subvol->path[root_len] != '/')) {
			error(
		"parent subvol is not reachable from inside the root subvol");
			ret = -ENOENT;
			goto out;
		}

		if (sub_len == root_len) {
			parent_subvol->path[0] = '.';
			parent_subvol->path[1] = '\0';
		} else {
			/*
			 * root path is foo/bar
			 * subvol path is foo/bar/baz
			 *
			 * we need to have baz be the path, so we need to move
			 * the bit after foo/bar/, so path + root_len + 1, and
			 * move the part we care about, so sub_len - root_len -
			 * 1.
			 */
			memmove(parent_subvol->path,
				parent_subvol->path + root_len + 1,
				sub_len - root_len - 1);
			parent_subvol->path[sub_len - root_len - 1] = '\0';
		}
	}

	if (*parent_subvol->path == 0)
		args_v2.fd = dup(rctx->mnt_fd);
	else
		args_v2.fd = openat(rctx->mnt_fd, parent_subvol->path,
				    O_RDONLY | O_NOATIME);
	if (args_v2.fd < 0) {
		ret = -errno;
		if (errno != ENOENT)
			error("cannot open %s: %m", parent_subvol->path);
		else
			fprintf(stderr,
				"It seems that you have changed your default "
				"subvolume or you specify other subvolume to\n"
				"mount btrfs, try to remount this btrfs filesystem "
				"with fs tree, and run btrfs receive again!\n");
		goto out;
	}

	ret = ioctl(rctx->dest_dir_fd, BTRFS_IOC_SNAP_CREATE_V2, &args_v2);
	close(args_v2.fd);
	if (ret < 0) {
		ret = -errno;
		error("creating snapshot %s -> %s failed: %m",
				parent_subvol->path, path);
		goto out;
	}

out:
	if (!IS_ERR_OR_NULL(parent_subvol)) {
		free(parent_subvol->path);
		free(parent_subvol);
	}
	return ret;
}

static int process_mkfile(const char *path, void *user)
{
	int ret;
	struct btrfs_receive *rctx = user;
	char full_path[PATH_MAX];

	ret = path_cat_out(full_path, rctx->full_subvol_path, path);
	if (ret < 0) {
		error("mkfile: path invalid: %s", path);
		goto out;
	}

	if (bconf.verbose >= 3)
		fprintf(stderr, "mkfile %s\n", path);

	ret = creat(full_path, 0600);
	if (ret < 0) {
		ret = -errno;
		error("mkfile %s failed: %m", path);
		goto out;
	}
	close(ret);
	ret = 0;

out:
	return ret;
}

static int process_mkdir(const char *path, void *user)
{
	int ret;
	struct btrfs_receive *rctx = user;
	char full_path[PATH_MAX];

	ret = path_cat_out(full_path, rctx->full_subvol_path, path);
	if (ret < 0) {
		error("mkdir: path invalid: %s", path);
		goto out;
	}

	if (bconf.verbose >= 3)
		fprintf(stderr, "mkdir %s\n", path);

	ret = mkdir(full_path, 0700);
	if (ret < 0) {
		ret = -errno;
		error("mkdir %s failed: %m", path);
	}

out:
	return ret;
}

static int process_mknod(const char *path, u64 mode, u64 dev, void *user)
{
	int ret;
	struct btrfs_receive *rctx = user;
	char full_path[PATH_MAX];

	ret = path_cat_out(full_path, rctx->full_subvol_path, path);
	if (ret < 0) {
		error("mknod: path invalid: %s", path);
		goto out;
	}

	if (bconf.verbose >= 3)
		fprintf(stderr, "mknod %s mode=%llu, dev=%llu\n",
				path, mode, dev);

	ret = mknod(full_path, mode & S_IFMT, dev);
	if (ret < 0) {
		ret = -errno;
		error("mknod %s failed: %m", path);
	}

out:
	return ret;
}

static int process_mkfifo(const char *path, void *user)
{
	int ret;
	struct btrfs_receive *rctx = user;
	char full_path[PATH_MAX];

	ret = path_cat_out(full_path, rctx->full_subvol_path, path);
	if (ret < 0) {
		error("mkfifo: path invalid: %s", path);
		goto out;
	}

	if (bconf.verbose >= 3)
		fprintf(stderr, "mkfifo %s\n", path);

	ret = mkfifo(full_path, 0600);
	if (ret < 0) {
		ret = -errno;
		error("mkfifo %s failed: %m", path);
	}

out:
	return ret;
}

static int process_mksock(const char *path, void *user)
{
	int ret;
	struct btrfs_receive *rctx = user;
	char full_path[PATH_MAX];

	ret = path_cat_out(full_path, rctx->full_subvol_path, path);
	if (ret < 0) {
		error("mksock: path invalid: %s", path);
		goto out;
	}

	if (bconf.verbose >= 3)
		fprintf(stderr, "mksock %s\n", path);

	ret = mknod(full_path, 0600 | S_IFSOCK, 0);
	if (ret < 0) {
		ret = -errno;
		error("mknod %s failed: %m", path);
	}

out:
	return ret;
}

static int process_symlink(const char *path, const char *lnk, void *user)
{
	int ret;
	struct btrfs_receive *rctx = user;
	char full_path[PATH_MAX];

	ret = path_cat_out(full_path, rctx->full_subvol_path, path);
	if (ret < 0) {
		error("symlink: path invalid: %s", path);
		goto out;
	}

	if (bconf.verbose >= 3)
		fprintf(stderr, "symlink %s -> %s\n", path, lnk);

	ret = symlink(lnk, full_path);
	if (ret < 0) {
		ret = -errno;
		error("symlink %s -> %s failed: %m", path, lnk);
	}

out:
	return ret;
}

static int process_rename(const char *from, const char *to, void *user)
{
	int ret;
	struct btrfs_receive *rctx = user;
	char full_from[PATH_MAX];
	char full_to[PATH_MAX];

	ret = path_cat_out(full_from, rctx->full_subvol_path, from);
	if (ret < 0) {
		error("rename: source path invalid: %s", from);
		goto out;
	}

	ret = path_cat_out(full_to, rctx->full_subvol_path, to);
	if (ret < 0) {
		error("rename: target path invalid: %s", to);
		goto out;
	}

	if (bconf.verbose >= 3)
		fprintf(stderr, "rename %s -> %s\n", from, to);

	ret = rename(full_from, full_to);
	if (ret < 0) {
		ret = -errno;
		error("rename %s -> %s failed: %m", from, to);
	}

out:
	return ret;
}

static int process_link(const char *path, const char *lnk, void *user)
{
	int ret;
	struct btrfs_receive *rctx = user;
	char full_path[PATH_MAX];
	char full_link_path[PATH_MAX];

	ret = path_cat_out(full_path, rctx->full_subvol_path, path);
	if (ret < 0) {
		error("link: source path invalid: %s", full_path);
		goto out;
	}

	ret = path_cat_out(full_link_path, rctx->full_subvol_path, lnk);
	if (ret < 0) {
		error("link: target path invalid: %s", full_link_path);
		goto out;
	}

	if (bconf.verbose >= 3)
		fprintf(stderr, "link %s -> %s\n", path, lnk);

	ret = link(full_link_path, full_path);
	if (ret < 0) {
		ret = -errno;
		error("link %s -> %s failed: %m", path, lnk);
	}

out:
	return ret;
}


static int process_unlink(const char *path, void *user)
{
	int ret;
	struct btrfs_receive *rctx = user;
	char full_path[PATH_MAX];

	ret = path_cat_out(full_path, rctx->full_subvol_path, path);
	if (ret < 0) {
		error("unlink: path invalid: %s", path);
		goto out;
	}

	if (bconf.verbose >= 3)
		fprintf(stderr, "unlink %s\n", path);

	ret = unlink(full_path);
	if (ret < 0) {
		ret = -errno;
		error("unlink %s failed: %m", path);
	}

out:
	return ret;
}

static int process_rmdir(const char *path, void *user)
{
	int ret;
	struct btrfs_receive *rctx = user;
	char full_path[PATH_MAX];

	ret = path_cat_out(full_path, rctx->full_subvol_path, path);
	if (ret < 0) {
		error("rmdir: path invalid: %s", path);
		goto out;
	}

	if (bconf.verbose >= 3)
		fprintf(stderr, "rmdir %s\n", path);

	ret = rmdir(full_path);
	if (ret < 0) {
		ret = -errno;
		error("rmdir %s failed: %m", path);
	}

out:
	return ret;
}

static int open_inode_for_write(struct btrfs_receive *rctx, const char *path)
{
	int ret = 0;

	if (rctx->write_fd != -1) {
		if (strcmp(rctx->write_path, path) == 0)
			goto out;
		close(rctx->write_fd);
		rctx->write_fd = -1;
	}

	rctx->write_fd = open(path, O_RDWR);
	if (rctx->write_fd < 0) {
		ret = -errno;
		error("cannot open %s: %m", path);
		goto out;
	}
	strncpy_null(rctx->write_path, path);

out:
	return ret;
}

static void close_inode_for_write(struct btrfs_receive *rctx)
{
	if(rctx->write_fd == -1)
		return;

	close(rctx->write_fd);
	rctx->write_fd = -1;
	rctx->write_path[0] = 0;
}

static int process_write(const char *path, const void *data, u64 offset,
			 u64 len, void *user)
{
	int ret = 0;
	struct btrfs_receive *rctx = user;
	char full_path[PATH_MAX];
	u64 pos = 0;
	int w;

	ret = path_cat_out(full_path, rctx->full_subvol_path, path);
	if (ret < 0) {
		error("write: path invalid: %s", path);
		goto out;
	}

	ret = open_inode_for_write(rctx, full_path);
	if (ret < 0)
		goto out;

	if (bconf.verbose >= 2)
		fprintf(stderr, "write %s - offset=%llu length=%llu\n",
			path, offset, len);

	while (pos < len) {
		w = pwrite(rctx->write_fd, (char*)data + pos, len - pos,
				offset + pos);
		if (w < 0) {
			ret = -errno;
			error("writing to %s failed: %m", path);
			goto out;
		}
		pos += w;
	}

out:
	return ret;
}

static int process_clone(const char *path, u64 offset, u64 len,
			 const u8 *clone_uuid, u64 clone_ctransid,
			 const char *clone_path, u64 clone_offset,
			 void *user)
{
	int ret;
	struct btrfs_receive *rctx = user;
	struct btrfs_ioctl_clone_range_args clone_args;
	struct subvol_info *si = NULL;
	char full_path[PATH_MAX];
	const char *subvol_path;
	char full_clone_path[PATH_MAX];
	int clone_fd = -1;

	ret = path_cat_out(full_path, rctx->full_subvol_path, path);
	if (ret < 0) {
		error("clone: source path invalid: %s", path);
		goto out;
	}

	ret = open_inode_for_write(rctx, full_path);
	if (ret < 0)
		goto out;

	if (memcmp(clone_uuid, rctx->cur_subvol.received_uuid,
		   BTRFS_UUID_SIZE) == 0) {
		subvol_path = rctx->cur_subvol_path;
	} else {
		si = subvol_uuid_search(rctx->mnt_fd, 0, clone_uuid, clone_ctransid,
					NULL,
					subvol_search_by_received_uuid);
		if (IS_ERR_OR_NULL(si)) {
			if (!si)
				ret = -ENOENT;
			else
				ret = PTR_ERR(si);
			error("clone: did not find source subvol");
			goto out;
		}
		/* strip the subvolume that we are receiving to from the start of subvol_path */
		if (rctx->full_root_path) {
			size_t root_len = strlen(rctx->full_root_path);
			size_t sub_len = strlen(si->path);

			if (sub_len > root_len &&
			    strstr(si->path, rctx->full_root_path) == si->path &&
			    si->path[root_len] == '/') {
				subvol_path = si->path + root_len + 1;
			} else {
				error("clone: source subvol path %s unreachable from %s",
					si->path, rctx->full_root_path);
				goto out;
			}
		} else {
			subvol_path = si->path;
		}
	}

	ret = path_cat_out(full_clone_path, subvol_path, clone_path);
	if (ret < 0) {
		error("clone: target path invalid: %s", clone_path);
		goto out;
	}

	clone_fd = openat(rctx->mnt_fd, full_clone_path, O_RDONLY | O_NOATIME);
	if (clone_fd < 0) {
		ret = -errno;
		error("cannot open %s: %m", full_clone_path);
		goto out;
	}

	if (bconf.verbose >= 2)
		fprintf(stderr,
			"clone %s - source=%s source offset=%llu offset=%llu length=%llu\n",
			path, clone_path, clone_offset, offset, len);

	clone_args.src_fd = clone_fd;
	clone_args.src_offset = clone_offset;
	clone_args.src_length = len;
	clone_args.dest_offset = offset;
	ret = ioctl(rctx->write_fd, BTRFS_IOC_CLONE_RANGE, &clone_args);
	if (ret < 0) {
		ret = -errno;
		error("failed to clone extents to %s: %m", path);
		goto out;
	}

out:
	if (!IS_ERR_OR_NULL(si)) {
		free(si->path);
		free(si);
	}
	if (clone_fd != -1)
		close(clone_fd);
	return ret;
}


static int process_set_xattr(const char *path, const char *name,
			     const void *data, int len, void *user)
{
	int ret = 0;
	struct btrfs_receive *rctx = user;
	char full_path[PATH_MAX];

	ret = path_cat_out(full_path, rctx->full_subvol_path, path);
	if (ret < 0) {
		error("set_xattr: path invalid: %s", path);
		goto out;
	}

	if (bconf.verbose >= 3) {
		fprintf(stderr, "set_xattr %s - name=%s data_len=%d "
				"data=%.*s\n", path, name, len,
				len, (char*)data);
	}

	ret = lsetxattr(full_path, name, data, len, 0);
	if (ret < 0) {
		ret = -errno;
		error("lsetxattr %s %s=%.*s failed: %m",
				path, name, len, (char*)data);
		goto out;
	}

out:
	return ret;
}

static int process_remove_xattr(const char *path, const char *name, void *user)
{
	int ret = 0;
	struct btrfs_receive *rctx = user;
	char full_path[PATH_MAX];

	ret = path_cat_out(full_path, rctx->full_subvol_path, path);
	if (ret < 0) {
		error("remove_xattr: path invalid: %s", path);
		goto out;
	}

	if (bconf.verbose >= 3) {
		fprintf(stderr, "remove_xattr %s - name=%s\n",
				path, name);
	}

	ret = lremovexattr(full_path, name);
	if (ret < 0) {
		ret = -errno;
		error("lremovexattr %s %s failed: %m", path, name);
		goto out;
	}

out:
	return ret;
}

static int process_truncate(const char *path, u64 size, void *user)
{
	int ret = 0;
	struct btrfs_receive *rctx = user;
	char full_path[PATH_MAX];

	ret = path_cat_out(full_path, rctx->full_subvol_path, path);
	if (ret < 0) {
		error("truncate: path invalid: %s", path);
		goto out;
	}

	if (bconf.verbose >= 3)
		fprintf(stderr, "truncate %s size=%llu\n", path, size);

	ret = truncate(full_path, size);
	if (ret < 0) {
		ret = -errno;
		error("truncate %s failed: %m", path);
		goto out;
	}

out:
	return ret;
}

static int process_chmod(const char *path, u64 mode, void *user)
{
	int ret = 0;
	struct btrfs_receive *rctx = user;
	char full_path[PATH_MAX];

	ret = path_cat_out(full_path, rctx->full_subvol_path, path);
	if (ret < 0) {
		error("chmod: path invalid: %s", path);
		goto out;
	}

	if (bconf.verbose >= 3)
		fprintf(stderr, "chmod %s - mode=0%o\n", path, (int)mode);

	ret = chmod(full_path, mode);
	if (ret < 0) {
		ret = -errno;
		error("chmod %s failed: %m", path);
		goto out;
	}

out:
	return ret;
}

static int process_chown(const char *path, u64 uid, u64 gid, void *user)
{
	int ret = 0;
	struct btrfs_receive *rctx = user;
	char full_path[PATH_MAX];

	ret = path_cat_out(full_path, rctx->full_subvol_path, path);
	if (ret < 0) {
		error("chown: path invalid: %s", path);
		goto out;
	}

	if (bconf.verbose >= 3)
		fprintf(stderr, "chown %s - uid=%llu, gid=%llu\n", path,
				uid, gid);

	ret = lchown(full_path, uid, gid);
	if (ret < 0) {
		ret = -errno;
		error("chown %s failed: %m", path);
		goto out;
	}
out:
	return ret;
}

static int process_utimes(const char *path, struct timespec *at,
			  struct timespec *mt, struct timespec *ct,
			  void *user)
{
	int ret = 0;
	struct btrfs_receive *rctx = user;
	char full_path[PATH_MAX];
	struct timespec tv[2];

	ret = path_cat_out(full_path, rctx->full_subvol_path, path);
	if (ret < 0) {
		error("utimes: path invalid: %s", path);
		goto out;
	}

	if (bconf.verbose >= 3)
		fprintf(stderr, "utimes %s\n", path);

	tv[0] = *at;
	tv[1] = *mt;
	ret = utimensat(AT_FDCWD, full_path, tv, AT_SYMLINK_NOFOLLOW);
	if (ret < 0) {
		ret = -errno;
		error("utimes %s failed: %m", path);
		goto out;
	}

out:
	return ret;
}

static int process_update_extent(const char *path, u64 offset, u64 len,
		void *user)
{
	if (bconf.verbose >= 3)
		fprintf(stderr, "update_extent %s: offset=%llu, len=%llu\n",
				path, offset, len);

	/*
	 * Sent with BTRFS_SEND_FLAG_NO_FILE_DATA, nothing to do.
	 */

	return 0;
}

static int decompress_zlib(struct btrfs_receive *rctx, const char *encoded_data,
			   u64 encoded_len, char *unencoded_data,
			   u64 unencoded_len)
{
	bool init = false;
	int ret;

	if (!rctx->zlib_stream) {
		init = true;
		rctx->zlib_stream = malloc(sizeof(z_stream));
		if (!rctx->zlib_stream) {
			error_msg(ERROR_MSG_MEMORY, "zlib stream: %m");
			return -ENOMEM;
		}
	}
	rctx->zlib_stream->next_in = (void *)encoded_data;
	rctx->zlib_stream->avail_in = encoded_len;
	rctx->zlib_stream->next_out = (void *)unencoded_data;
	rctx->zlib_stream->avail_out = unencoded_len;

	if (init) {
		rctx->zlib_stream->zalloc = Z_NULL;
		rctx->zlib_stream->zfree = Z_NULL;
		rctx->zlib_stream->opaque = Z_NULL;
		ret = inflateInit(rctx->zlib_stream);
	} else {
		ret = inflateReset(rctx->zlib_stream);
	}
	if (ret != Z_OK) {
		error("zlib inflate init failed: %d", ret);
		return -EIO;
	}

	while (rctx->zlib_stream->avail_in > 0 &&
	       rctx->zlib_stream->avail_out > 0) {
		ret = inflate(rctx->zlib_stream, Z_FINISH);
		if (ret == Z_STREAM_END) {
			break;
		} else if (ret != Z_OK) {
			error("zlib inflate failed: %d", ret);
			return -EIO;
		}
	}
	return 0;
}

#if COMPRESSION_ZSTD
static int decompress_zstd(struct btrfs_receive *rctx, const char *encoded_buf,
			   u64 encoded_len, char *unencoded_buf,
			   u64 unencoded_len)
{
	ZSTD_inBuffer in_buf = {
		.src = encoded_buf,
		.size = encoded_len
	};
	ZSTD_outBuffer out_buf = {
		.dst = unencoded_buf,
		.size = unencoded_len
	};
	size_t ret;

	if (!rctx->zstd_dstream) {
		rctx->zstd_dstream = ZSTD_createDStream();
		if (!rctx->zstd_dstream) {
			error("failed to create zstd dstream");
			return -ENOMEM;
		}
	}
	ret = ZSTD_initDStream(rctx->zstd_dstream);
	if (ZSTD_isError(ret)) {
		error("failed to init zstd stream: %s", ZSTD_getErrorName(ret));
		return -EIO;
	}
	while (in_buf.pos < in_buf.size && out_buf.pos < out_buf.size) {
		ret = ZSTD_decompressStream(rctx->zstd_dstream, &out_buf, &in_buf);
		if (ret == 0) {
			break;
		} else if (ZSTD_isError(ret)) {
			error("failed to decompress zstd stream: %s",
			      ZSTD_getErrorName(ret));
			return -EIO;
		}
	}

	/*
	 * Zero out the unused part of the output buffer.
	 * At least with zstd 1.5.2, the decompression can leave some garbage
	 * at/beyond the offset out_buf.pos.
	 */
	if (out_buf.pos < out_buf.size)
		memset(unencoded_buf + out_buf.pos, 0, out_buf.size - out_buf.pos);

	return 0;
}
#endif

#if COMPRESSION_LZO
static int decompress_lzo(const char *encoded_data, u64 encoded_len,
			  char *unencoded_data, u64 unencoded_len,
			  unsigned int sector_size)
{
	uint32_t total_len;
	size_t in_pos, out_pos;

	if (encoded_len < 4) {
		error("lzo header is truncated");
		return -EIO;
	}
	memcpy(&total_len, encoded_data, 4);
	total_len = le32toh(total_len);
	if (total_len > encoded_len) {
		error("lzo header is invalid");
		return -EIO;
	}

	in_pos = 4;
	out_pos = 0;
	while (in_pos < total_len && out_pos < unencoded_len) {
		size_t sector_remaining;
		uint32_t src_len;
		lzo_uint dst_len;
		int ret;

		sector_remaining = -in_pos % sector_size;
		if (sector_remaining < 4) {
			if (total_len - in_pos <= sector_remaining)
				break;
			in_pos += sector_remaining;
		}

		if (total_len - in_pos < 4) {
			error("lzo segment header is truncated");
			return -EIO;
		}

		memcpy(&src_len, encoded_data + in_pos, 4);
		src_len = le32toh(src_len);
		in_pos += 4;
		if (src_len > total_len - in_pos) {
			error("lzo segment header is invalid");
			return -EIO;
		}

		dst_len = sector_size;
		ret = lzo1x_decompress_safe((void *)(encoded_data + in_pos),
					    src_len,
					    (void *)(unencoded_data + out_pos),
					    &dst_len, NULL);
		if (ret != LZO_E_OK) {
			error("lzo1x_decompress_safe failed: %d", ret);
			return -EIO;
		}

		in_pos += src_len;
		out_pos += dst_len;
	}
	return 0;
}
#endif

static int decompress_and_write(struct btrfs_receive *rctx,
				const char *encoded_data, u64 offset,
				u64 encoded_len, u64 unencoded_file_len,
				u64 unencoded_len, u64 unencoded_offset,
				u32 compression)
{
	int ret = 0;
	char *unencoded_data;
	int sector_shift = 0;
	u64 written = 0;

	unencoded_data = calloc(unencoded_len, 1);
	if (!unencoded_data) {
		error_msg(ERROR_MSG_MEMORY, "unencoded data: %m");
		return -errno;
	}

	switch (compression) {
	case BTRFS_ENCODED_IO_COMPRESSION_ZLIB:
		ret = decompress_zlib(rctx, encoded_data, encoded_len,
				      unencoded_data, unencoded_len);
		if (ret)
			goto out;
		break;
#if COMPRESSION_ZSTD
	case BTRFS_ENCODED_IO_COMPRESSION_ZSTD:
		ret = decompress_zstd(rctx, encoded_data, encoded_len,
				      unencoded_data, unencoded_len);
		if (ret)
			goto out;
		break;
#else
		error("ZSTD compression for stream not compiled in");
		ret = -EOPNOTSUPP;
		goto out;
#endif
	case BTRFS_ENCODED_IO_COMPRESSION_LZO_4K:
	case BTRFS_ENCODED_IO_COMPRESSION_LZO_8K:
	case BTRFS_ENCODED_IO_COMPRESSION_LZO_16K:
	case BTRFS_ENCODED_IO_COMPRESSION_LZO_32K:
	case BTRFS_ENCODED_IO_COMPRESSION_LZO_64K:
#if COMPRESSION_LZO
		sector_shift =
			compression - BTRFS_ENCODED_IO_COMPRESSION_LZO_4K + 12;
		ret = decompress_lzo(encoded_data, encoded_len, unencoded_data,
				     unencoded_len, 1U << sector_shift);
		if (ret)
			goto out;
		break;
#else
		error("LZO compression for stream not compiled in");
		ret = -EOPNOTSUPP;
		goto out;
#endif
	default:
		error("unknown compression: %d", compression);
		ret = -EOPNOTSUPP;
		goto out;
	}

	while (written < unencoded_file_len) {
		ssize_t w;

		w = pwrite(rctx->write_fd, unencoded_data + unencoded_offset,
			   unencoded_file_len - written, offset);
		if (w < 0) {
			ret = -errno;
			error("writing unencoded data failed: %m");
			goto out;
		}
		written += w;
		offset += w;
		unencoded_offset += w;
	}
out:
	free(unencoded_data);
	return ret;
}

static int process_encoded_write(const char *path, const void *data, u64 offset,
				 u64 len, u64 unencoded_file_len,
				 u64 unencoded_len, u64 unencoded_offset,
				 u32 compression, u32 encryption, void *user)
{
	int ret;
	struct btrfs_receive *rctx = user;
	char full_path[PATH_MAX];
	struct iovec iov = { (char *)data, len };
	struct btrfs_ioctl_encoded_io_args encoded = {
		.iov = &iov,
		.iovcnt = 1,
		.offset = offset,
		.len = unencoded_file_len,
		.unencoded_len = unencoded_len,
		.unencoded_offset = unencoded_offset,
		.compression = compression,
		.encryption = encryption,
	};

	if (bconf.verbose >= 3)
		fprintf(stderr,
"encoded_write %s - offset=%llu, len=%llu, unencoded_offset=%llu, unencoded_file_len=%llu, unencoded_len=%llu, compression=%u, encryption=%u\n",
			path, offset, len, unencoded_offset, unencoded_file_len,
			unencoded_len, compression, encryption);

	if (encryption) {
		error("encoded_write: encryption not supported");
		return -EOPNOTSUPP;
	}

	ret = path_cat_out(full_path, rctx->full_subvol_path, path);
	if (ret < 0) {
		error("encoded_write: path invalid: %s", path);
		return ret;
	}

	ret = open_inode_for_write(rctx, full_path);
	if (ret < 0)
		return ret;

	if (!rctx->force_decompress) {
		ret = ioctl(rctx->write_fd, BTRFS_IOC_ENCODED_WRITE, &encoded);
		if (ret >= 0)
			return 0;
		/* Fall back for these errors, fail hard for anything else. */
		if (errno != ENOSPC && errno != ENOTTY && errno != EINVAL) {
			ret = -errno;
			error("encoded_write: writing to %s failed: %m", path);
			return ret;
		}
		if (bconf.verbose >= 3)
			fprintf(stderr,
"encoded_write %s - falling back to decompress and write due to errno %d (\"%m\")\n",
				path, errno);
	}

	return decompress_and_write(rctx, data, offset, len, unencoded_file_len,
				    unencoded_len, unencoded_offset,
				    compression);
}

static int process_fallocate(const char *path, int mode, u64 offset, u64 len,
			     void *user)
{
	int ret;
	struct btrfs_receive *rctx = user;
	char full_path[PATH_MAX];

	if (bconf.verbose >= 3)
		fprintf(stderr,
			"fallocate %s - offset=%llu, len=%llu, mode=%d\n",
			path, offset, len, mode);

	ret = path_cat_out(full_path, rctx->full_subvol_path, path);
	if (ret < 0) {
		error("fallocate: path invalid: %s", path);
		return ret;
	}
	ret = open_inode_for_write(rctx, full_path);
	if (ret < 0)
		return ret;
	ret = fallocate(rctx->write_fd, mode, offset, len);
	if (ret < 0) {
		ret = -errno;
		error("fallocate: fallocate on %s failed: %m", path);
		return ret;
	}
	return 0;
}

static int process_fileattr(const char *path, u64 attr, void *user)
{
	/*
	 * Not yet supported, ignored for now, just like in send stream v1.
	 * The content of 'attr' matches the flags in the btrfs inode item,
	 * we can't apply them directly with FS_IOC_SETFLAGS, as we need to
	 * convert them from BTRFS_INODE_* flags to FS_* flags. Plus some
	 * flags are special and must be applied in a special way.
	 * The commented code below therefore does not work.
	 */

	/* int ret; */
	/* struct btrfs_receive *rctx = user; */
	/* char full_path[PATH_MAX]; */

	/* ret = path_cat_out(full_path, rctx->full_subvol_path, path); */
	/* if (ret < 0) { */
	/* 	error("fileattr: path invalid: %s", path); */
	/* 	return ret; */
	/* } */
	/* ret = open_inode_for_write(rctx, full_path); */
	/* if (ret < 0) */
	/* 	return ret; */
	/* ret = ioctl(rctx->write_fd, FS_IOC_SETFLAGS, &attr); */
	/* if (ret < 0) { */
	/* 	ret = -errno; */
	/* 	error("fileattr: set file attributes on %s failed: %m", path); */
	/* 	return ret; */
	/* } */
	return 0;
}

#if HAVE_LINUX_FSVERITY_H
static int process_enable_verity(const char *path, u8 algorithm, u32 block_size,
				 int salt_len, char *salt,
				 int sig_len, char *sig, void *user)
{
	int ret;
	int ioctl_fd;
	struct btrfs_receive *rctx = user;
	char full_path[PATH_MAX];
	struct fsverity_enable_arg verity_args = {
		.version = 1,
		.hash_algorithm = algorithm,
		.block_size = block_size,
	};

	if (salt_len) {
		verity_args.salt_size = salt_len;
		verity_args.salt_ptr = (__u64)(uintptr_t)salt;
	}

	if (sig_len) {
		verity_args.sig_size = sig_len;
		verity_args.sig_ptr = (__u64)(uintptr_t)sig;
	}

	ret = path_cat_out(full_path, rctx->full_subvol_path, path);
	if (ret < 0) {
		error("write: path invalid: %s", path);
		goto out;
	}

	ioctl_fd = open(full_path, O_RDONLY);
	if (ioctl_fd < 0) {
		ret = -errno;
		error("cannot open %s for verity ioctl: %m", full_path);
		goto out;
	}

	/*
	 * Enabling verity denies write access, so it cannot be done with an
	 * open writeable file descriptor.
	 */
	close_inode_for_write(rctx);
	ret = ioctl(ioctl_fd, FS_IOC_ENABLE_VERITY, &verity_args);
	if (ret < 0) {
		ret = -errno;
		error("failed to enable verity on %s: %d", full_path, ret);
	}

	close(ioctl_fd);
out:
	return ret;
}

#else

static int process_enable_verity(const char *path, u8 algorithm, u32 block_size,
				 int salt_len, char *salt,
				 int sig_len, char *sig, void *user)
{
	error("fs-verity for stream not compiled in");
	return -EOPNOTSUPP;
}

#endif

static struct btrfs_send_ops send_ops = {
	.subvol = process_subvol,
	.snapshot = process_snapshot,
	.mkfile = process_mkfile,
	.mkdir = process_mkdir,
	.mknod = process_mknod,
	.mkfifo = process_mkfifo,
	.mksock = process_mksock,
	.symlink = process_symlink,
	.rename = process_rename,
	.link = process_link,
	.unlink = process_unlink,
	.rmdir = process_rmdir,
	.write = process_write,
	.clone = process_clone,
	.set_xattr = process_set_xattr,
	.remove_xattr = process_remove_xattr,
	.truncate = process_truncate,
	.chmod = process_chmod,
	.chown = process_chown,
	.utimes = process_utimes,
	.update_extent = process_update_extent,
	.encoded_write = process_encoded_write,
	.fallocate = process_fallocate,
	.fileattr = process_fileattr,
	.enable_verity = process_enable_verity,
};

static int do_receive(struct btrfs_receive *rctx, const char *tomnt,
		      char *realmnt, int r_fd, u64 max_errors)
{
	u64 subvol_id;
	int ret;
	char *dest_dir_full_path;
	char root_subvol_path[PATH_MAX];
	bool end = false;
	int iterations = 0;

	dest_dir_full_path = realpath(tomnt, NULL);
	if (!dest_dir_full_path) {
		ret = -errno;
		error("realpath(%s) failed: %m", tomnt);
		goto out;
	}
	rctx->dest_dir_fd = open(dest_dir_full_path, O_RDONLY | O_NOATIME);
	if (rctx->dest_dir_fd < 0) {
		ret = -errno;
		error("cannot open destination directory %s: %m",
			dest_dir_full_path);
		goto out;
	}

	if (realmnt[0]) {
		rctx->root_path = realmnt;
	} else {
		ret = find_mount_root(dest_dir_full_path, &rctx->root_path);
		if (ret < 0) {
			errno = -ret;
			error("failed to determine mount point for %s: %m",
				dest_dir_full_path);
			ret = -EINVAL;
			goto out;
		}
		if (ret > 0) {
			error("%s doesn't belong to btrfs mount point",
				dest_dir_full_path);
			ret = -EINVAL;
			goto out;
		}
	}
	rctx->mnt_fd = open(rctx->root_path, O_RDONLY | O_NOATIME);
	if (rctx->mnt_fd < 0) {
		ret = -errno;
		error("cannot open %s: %m", rctx->root_path);
		goto out;
	}

	/*
	 * If we use -m or a default subvol we want to resolve the path to the
	 * subvolume we're sitting in so that we can adjust the paths of any
	 * subvols we want to receive in.
	 */
	ret = lookup_path_rootid(rctx->mnt_fd, &subvol_id);
	if (ret) {
		errno = -ret;
		error("cannot resolve rootid for path: %m");
		goto out;
	}

	root_subvol_path[0] = 0;
	ret = btrfs_subvolid_resolve(rctx->mnt_fd, root_subvol_path,
				     PATH_MAX, subvol_id);
	if (ret) {
		error("cannot resolve our subvol path");
		goto out;
	}

	/*
	 * Ok we're inside of a subvol off of the root subvol, we need to
	 * actually set full_root_path.
	 */
	if (*root_subvol_path)
		rctx->full_root_path = root_subvol_path;

	if (rctx->dest_dir_chroot) {
		if (chroot(dest_dir_full_path)) {
			ret = -errno;
			error("failed to chroot to %s: %m", dest_dir_full_path);
			goto out;
		}
		if (chdir("/")) {
			ret = -errno;
			error("failed to chdir to / after chroot: %m");
			goto out;
		}
		fprintf(stderr, "Chroot to %s\n", dest_dir_full_path);
		rctx->root_path = strdup("/");
		rctx->dest_dir_path = rctx->root_path;
	} else {
		/*
		 * find_mount_root returns a root_path that is a subpath of
		 * dest_dir_full_path. Now get the other part of root_path,
		 * which is the destination dir relative to root_path.
		 */
		rctx->dest_dir_path = dest_dir_full_path + strlen(rctx->root_path);
		while (rctx->dest_dir_path[0] == '/')
			rctx->dest_dir_path++;
	}

	while (!end) {
		ret = btrfs_read_and_process_send_stream(r_fd, &send_ops,
							 rctx,
							 rctx->honor_end_cmd,
							 max_errors);
		if (ret < 0) {
			if (ret != -ENODATA)
				goto out;

			/* Empty stream is invalid */
			if (iterations == 0) {
				error("empty stream is not considered valid");
				ret = -EINVAL;
				goto out;
			}

			ret = 1;
		}
		if (ret > 0)
			end = true;

		close_inode_for_write(rctx);
		ret = finish_subvol(rctx);
		if (ret < 0)
			goto out;

		iterations++;
	}
	ret = 0;

out:
	if (rctx->write_fd != -1) {
		close(rctx->write_fd);
		rctx->write_fd = -1;
	}

	if (rctx->root_path != realmnt)
		free(rctx->root_path);
	rctx->root_path = NULL;
	rctx->dest_dir_path = NULL;
	free(dest_dir_full_path);
	if (rctx->mnt_fd != -1) {
		close(rctx->mnt_fd);
		rctx->mnt_fd = -1;
	}
	if (rctx->dest_dir_fd != -1) {
		close(rctx->dest_dir_fd);
		rctx->dest_dir_fd = -1;
	}
#if COMPRESSION_ZSTD
	if (rctx->zstd_dstream)
		ZSTD_freeDStream(rctx->zstd_dstream);
#endif
	if (rctx->zlib_stream) {
		inflateEnd(rctx->zlib_stream);
		free(rctx->zlib_stream);
	}

	return ret;
}

static const char * const cmd_receive_usage[] = {
	"btrfs receive [options] <mount>\n"
	"btrfs receive --dump [options]",
	"Receive subvolumes from a stream",
	"Receives one or more subvolumes that were previously",
	"sent with btrfs send. The received subvolumes are stored",
	"into MOUNT.",
	"The receive will fail in case the receiving subvolume",
	"already exists. It will also fail in case a previously",
	"received subvolume has been changed after it was received.",
	"After receiving a subvolume, it is immediately set to",
	"read-only.",
	"",
	OPTLINE("-q|--quiet", "suppress all messages, except errors"),
	OPTLINE("-f FILE", "read the stream from FILE instead of stdin"),
	OPTLINE("-e", "terminate after receiving an <end cmd> marker in the stream. "
		"Without this option the receiver side terminates only in case "
		"of an error on end of file."),
	OPTLINE("-C|--chroot", "confine the process to <mount> using chroot"),
	OPTLINE("-E|--max-errors NERR", "terminate as soon as NERR errors occur while "
		"stream processing commands from the stream. "
		"Default value is 1. A value of 0 means no limit."),
	OPTLINE("-m ROOTMOUNT", "the root mount point of the destination filesystem. "
		"If /proc is not accessible, use this to tell us where "
		"this file system is mounted."),
	OPTLINE("--force-decompress", "if the stream contains compressed data, always "
		"decompress it instead of writing it with encoded I/O"),
	OPTLINE("--dump", "dump stream metadata, one line per operation, "
		"does not require the MOUNT parameter"),
	OPTLINE("-v", "deprecated, alias for global -v option"),
	HELPINFO_INSERT_GLOBALS,
	HELPINFO_INSERT_VERBOSE,
	HELPINFO_INSERT_QUIET,
	"",
	"Compression support: zlib"
#if COMPRESSION_LZO
		", lzo"
#endif
#if COMPRESSION_ZSTD
		", zstd"
#endif
	,
	"Feature support:"
#if HAVE_LINUX_FSVERITY_H
		" fsverity"
#endif
	,
	NULL
};

static int cmd_receive(const struct cmd_struct *cmd, int argc, char **argv)
{
	char *tomnt = NULL;
	char fromfile[PATH_MAX];
	char realmnt[PATH_MAX];
	struct btrfs_receive rctx;
	int receive_fd = fileno(stdin);
	u64 max_errors = 1;
	bool dump = false;
	int ret = 0;

	memset(&rctx, 0, sizeof(rctx));
	rctx.mnt_fd = -1;
	rctx.write_fd = -1;
	rctx.dest_dir_fd = -1;
	rctx.dest_dir_chroot = false;
	realmnt[0] = 0;
	fromfile[0] = 0;

	/*
	 * Init global verbose to default, if it is unset.
	 * Default is 1 for historical reasons, changing may break scripts that
	 * expect the 'At subvol' message.
	 * As default is 1, which means the effective verbose for receive is 2
	 * which global verbose is unaware. So adjust global verbose value here.
	 */
	if (bconf.verbose == BTRFS_BCONF_UNSET)
		bconf.verbose = 1;
	else if (bconf.verbose > BTRFS_BCONF_QUIET)
		bconf.verbose++;

	optind = 0;
	while (1) {
		int c;
		enum {
			GETOPT_VAL_DUMP = GETOPT_VAL_FIRST,
			GETOPT_VAL_FORCE_DECOMPRESS,
		};
		static const struct option long_opts[] = {
			{ "max-errors", required_argument, NULL, 'E' },
			{ "chroot", no_argument, NULL, 'C' },
			{ "dump", no_argument, NULL, GETOPT_VAL_DUMP },
			{ "quiet", no_argument, NULL, 'q' },
			{ "force-decompress", no_argument, NULL, GETOPT_VAL_FORCE_DECOMPRESS },
			{ NULL, 0, NULL, 0 }
		};

		c = getopt_long(argc, argv, "Cevqf:m:E:", long_opts, NULL);
		if (c < 0)
			break;

		switch (c) {
		case 'v':
			bconf_be_verbose();
			break;
		case 'q':
			bconf_be_quiet();
			break;
		case 'f':
			if (arg_copy_path(fromfile, optarg, sizeof(fromfile))) {
				error("input file path too long (%zu)",
					strlen(optarg));
				ret = 1;
				goto out;
			}
			break;
		case 'e':
			rctx.honor_end_cmd = true;
			break;
		case 'C':
			rctx.dest_dir_chroot = true;
			break;
		case 'E':
			max_errors = arg_strtou64(optarg);
			break;
		case 'm':
			if (arg_copy_path(realmnt, optarg, sizeof(realmnt))) {
				error("mount point path too long (%zu)",
					strlen(optarg));
				ret = 1;
				goto out;
			}
			break;
		case GETOPT_VAL_DUMP:
			dump = true;
			break;
		case GETOPT_VAL_FORCE_DECOMPRESS:
			rctx.force_decompress = true;
			break;
		default:
			usage_unknown_option(cmd, argv);
		}
	}

	if (dump && check_argc_exact(argc - optind, 0))
		usage(cmd, 1);
	if (!dump && check_argc_exact(argc - optind, 1))
		usage(cmd, 1);

	tomnt = argv[optind];

	if (fromfile[0]) {
		receive_fd = open(fromfile, O_RDONLY | O_NOATIME);
		if (receive_fd < 0) {
			error("cannot open %s: %m", fromfile);
			goto out;
		}
	}

	if (dump) {
		struct btrfs_dump_send_args dump_args;

		dump_args.root_path[0] = '.';
		dump_args.root_path[1] = '\0';
		dump_args.full_subvol_path[0] = '.';
		dump_args.full_subvol_path[1] = '\0';
		ret = btrfs_read_and_process_send_stream(receive_fd,
			&btrfs_print_send_ops, &dump_args, 0, max_errors);
		if (ret < 0) {
			errno = -ret;
			error("failed to dump the send stream: %m");
		}
	} else {
		ret = do_receive(&rctx, tomnt, realmnt, receive_fd, max_errors);
	}

	if (receive_fd != fileno(stdin))
		close(receive_fd);
out:

	return !!ret;
}
DEFINE_SIMPLE_COMMAND(receive, "receive");
