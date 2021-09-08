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

#include <unistd.h>
#include <stdint.h>
#include <dirent.h>
#include <fcntl.h>
#include <pthread.h>
#include <math.h>
#include <ftw.h>
#include <sys/wait.h>
#include <assert.h>
#include <getopt.h>
#include <limits.h>

#include <sys/stat.h>
#include <sys/types.h>
#include <sys/ioctl.h>
#include <sys/time.h>
#include <sys/types.h>
#include <sys/xattr.h>
#include <uuid/uuid.h>

#include "kernel-shared/ctree.h"
#include "ioctl.h"
#include "cmds/commands.h"
#include "common/utils.h"
#include "kernel-lib/list.h"
#include "btrfs-list.h"

#include "kernel-shared/send.h"
#include "common/send-stream.h"
#include "common/send-utils.h"
#include "cmds/receive-dump.h"
#include "common/help.h"
#include "common/path-utils.h"

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
	int dest_dir_chroot;

	struct subvol_info cur_subvol;
	/*
	 * Substitute for cur_subvol::path which is a pointer and we cannot
	 * change it to an array as it's a public API.
	 */
	char cur_subvol_path[PATH_MAX];

	struct subvol_uuid_search sus;

	int honor_end_cmd;
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

	parent_subvol = subvol_uuid_search(&rctx->sus, 0, parent_uuid,
					   parent_ctransid, NULL,
					   subvol_search_by_received_uuid);
	if (IS_ERR_OR_NULL(parent_subvol)) {
		parent_subvol = subvol_uuid_search(&rctx->sus, 0, parent_uuid,
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
	if (parent_subvol) {
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
		si = subvol_uuid_search(&rctx->sus, 0, clone_uuid, clone_ctransid,
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
	if (si) {
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
				path, (unsigned long long)offset,
				(unsigned long long)len);

	/*
	 * Sent with BTRFS_SEND_FLAG_NO_FILE_DATA, nothing to do.
	 */

	return 0;
}

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
};

static int do_receive(struct btrfs_receive *rctx, const char *tomnt,
		      char *realmnt, int r_fd, u64 max_errors)
{
	u64 subvol_id;
	int ret;
	char *dest_dir_full_path;
	char root_subvol_path[PATH_MAX];
	int end = 0;
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

	ret = subvol_uuid_search_init(rctx->mnt_fd, &rctx->sus);
	if (ret < 0)
		goto out;

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
			end = 1;

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
	subvol_uuid_search_finit(&rctx->sus);
	if (rctx->mnt_fd != -1) {
		close(rctx->mnt_fd);
		rctx->mnt_fd = -1;
	}
	if (rctx->dest_dir_fd != -1) {
		close(rctx->dest_dir_fd);
		rctx->dest_dir_fd = -1;
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
	"-q|--quiet       suppress all messages, except errors",
	"-f FILE          read the stream from FILE instead of stdin",
	"-e               terminate after receiving an <end cmd> marker in the stream.",
	"                 Without this option the receiver side terminates only in case",
	"                 of an error on end of file.",
	"-C|--chroot      confine the process to <mount> using chroot",
	"-E|--max-errors NERR",
	"                 terminate as soon as NERR errors occur while",
	"                 stream processing commands from the stream.",
	"                 Default value is 1. A value of 0 means no limit.",
	"-m ROOTMOUNT     the root mount point of the destination filesystem.",
	"                 If /proc is not accessible, use this to tell us where",
	"                 this file system is mounted.",
	"--dump           dump stream metadata, one line per operation,",
	"                 does not require the MOUNT parameter",
	"-v               deprecated, alias for global -v option",
	HELPINFO_INSERT_GLOBALS,
	HELPINFO_INSERT_VERBOSE,
	HELPINFO_INSERT_QUIET,
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
	int dump = 0;
	int ret = 0;

	memset(&rctx, 0, sizeof(rctx));
	rctx.mnt_fd = -1;
	rctx.write_fd = -1;
	rctx.dest_dir_fd = -1;
	rctx.dest_dir_chroot = 0;
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
		enum { GETOPT_VAL_DUMP = 257 };
		static const struct option long_opts[] = {
			{ "max-errors", required_argument, NULL, 'E' },
			{ "chroot", no_argument, NULL, 'C' },
			{ "dump", no_argument, NULL, GETOPT_VAL_DUMP },
			{ "quiet", no_argument, NULL, 'q' },
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
			rctx.honor_end_cmd = 1;
			break;
		case 'C':
			rctx.dest_dir_chroot = 1;
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
			dump = 1;
			break;
		default:
			usage_unknown_option(cmd, argv);
		}
	}

	if (dump && check_argc_exact(argc - optind, 0))
		usage(cmd);
	if (!dump && check_argc_exact(argc - optind, 1))
		usage(cmd);

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
