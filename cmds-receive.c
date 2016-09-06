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
#include "androidcompat.h"

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

#include "ctree.h"
#include "ioctl.h"
#include "commands.h"
#include "utils.h"
#include "list.h"
#include "btrfs-list.h"

#include "send.h"
#include "send-stream.h"
#include "send-utils.h"

static int g_verbose = 0;

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

	/*
	 * Buffer to store capabilities from security.capabilities xattr,
	 * usually 20 bytes, but make same room for potentially larger
	 * encodings. Must be set only once per file, denoted by length > 0.
	 */
	char cached_capabilities[64];
	int cached_capabilities_len;
};

static int finish_subvol(struct btrfs_receive *r)
{
	int ret;
	int subvol_fd = -1;
	struct btrfs_ioctl_received_subvol_args rs_args;
	char uuid_str[BTRFS_UUID_UNPARSED_SIZE];
	u64 flags;

	if (r->cur_subvol_path[0] == 0)
		return 0;

	subvol_fd = openat(r->mnt_fd, r->cur_subvol_path,
			O_RDONLY | O_NOATIME);
	if (subvol_fd < 0) {
		ret = -errno;
		error("cannot open %s: %s\n",
				r->cur_subvol_path, strerror(-ret));
		goto out;
	}

	memset(&rs_args, 0, sizeof(rs_args));
	memcpy(rs_args.uuid, r->cur_subvol.received_uuid, BTRFS_UUID_SIZE);
	rs_args.stransid = r->cur_subvol.stransid;

	if (g_verbose >= 1) {
		uuid_unparse((u8*)rs_args.uuid, uuid_str);
		fprintf(stderr, "BTRFS_IOC_SET_RECEIVED_SUBVOL uuid=%s, "
				"stransid=%llu\n", uuid_str, rs_args.stransid);
	}

	ret = ioctl(subvol_fd, BTRFS_IOC_SET_RECEIVED_SUBVOL, &rs_args);
	if (ret < 0) {
		ret = -errno;
		error("ioctl BTRFS_IOC_SET_RECEIVED_SUBVOL failed: %s",
				strerror(-ret));
		goto out;
	}
	r->cur_subvol.rtransid = rs_args.rtransid;

	ret = ioctl(subvol_fd, BTRFS_IOC_SUBVOL_GETFLAGS, &flags);
	if (ret < 0) {
		ret = -errno;
		error("ioctl BTRFS_IOC_SUBVOL_GETFLAGS failed: %s",
				strerror(-ret));
		goto out;
	}

	flags |= BTRFS_SUBVOL_RDONLY;

	ret = ioctl(subvol_fd, BTRFS_IOC_SUBVOL_SETFLAGS, &flags);
	if (ret < 0) {
		ret = -errno;
		error("failed to make subvolume read only: %s",
				strerror(-ret));
		goto out;
	}

	ret = 0;

out:
	if (r->cur_subvol_path[0]) {
		r->cur_subvol_path[0] = 0;
	}
	if (subvol_fd != -1)
		close(subvol_fd);
	return ret;
}

static int process_subvol(const char *path, const u8 *uuid, u64 ctransid,
			  void *user)
{
	int ret;
	struct btrfs_receive *r = user;
	struct btrfs_ioctl_vol_args args_v1;
	char uuid_str[BTRFS_UUID_UNPARSED_SIZE];

	ret = finish_subvol(r);
	if (ret < 0)
		goto out;

	if (r->cur_subvol.path) {
		error("subvol: another one already started, path ptr: %s",
				r->cur_subvol.path);
		ret = -EINVAL;
		goto out;
	}
	if (r->cur_subvol_path[0]) {
		error("subvol: another one already started, path buf: %s",
				r->cur_subvol.path);
		ret = -EINVAL;
		goto out;
	}

	if (*r->dest_dir_path == 0) {
		strncpy_null(r->cur_subvol_path, path);
	} else {
		ret = path_cat_out(r->cur_subvol_path, r->dest_dir_path, path);
		if (ret < 0) {
			error("subvol: path invalid: %s\n", path);
			goto out;
		}
	}
	ret = path_cat3_out(r->full_subvol_path, r->root_path,
			r->dest_dir_path, path);
	if (ret < 0) {
		error("subvol: path invalid: %s", path);
		goto out;
	}

	fprintf(stderr, "At subvol %s\n", path);

	memcpy(r->cur_subvol.received_uuid, uuid, BTRFS_UUID_SIZE);
	r->cur_subvol.stransid = ctransid;

	if (g_verbose) {
		uuid_unparse((u8*)r->cur_subvol.received_uuid, uuid_str);
		fprintf(stderr, "receiving subvol %s uuid=%s, stransid=%llu\n",
				path, uuid_str,
				r->cur_subvol.stransid);
	}

	memset(&args_v1, 0, sizeof(args_v1));
	strncpy_null(args_v1.name, path);
	ret = ioctl(r->dest_dir_fd, BTRFS_IOC_SUBVOL_CREATE, &args_v1);
	if (ret < 0) {
		ret = -errno;
		error("creating subvolume %s failed: %s", path, strerror(-ret));
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
	struct btrfs_receive *r = user;
	char uuid_str[BTRFS_UUID_UNPARSED_SIZE];
	struct btrfs_ioctl_vol_args_v2 args_v2;
	struct subvol_info *parent_subvol = NULL;

	ret = finish_subvol(r);
	if (ret < 0)
		goto out;

	if (r->cur_subvol.path) {
		error("snapshot: another one already started, path ptr: %s",
				r->cur_subvol.path);
		ret = -EINVAL;
		goto out;
	}
	if (r->cur_subvol_path[0]) {
		error("snapshot: another one already started, path buf: %s",
				r->cur_subvol.path);
		ret = -EINVAL;
		goto out;
	}

	if (*r->dest_dir_path == 0) {
		strncpy_null(r->cur_subvol_path, path);
	} else {
		ret = path_cat_out(r->cur_subvol_path, r->dest_dir_path, path);
		if (ret < 0) {
			error("snapshot: path invalid: %s", path);
			goto out;
		}
	}
	ret = path_cat3_out(r->full_subvol_path, r->root_path,
			r->dest_dir_path, path);
	if (ret < 0) {
		error("snapshot: path invalid: %s", path);
		goto out;
	}

	fprintf(stdout, "At snapshot %s\n", path);

	memcpy(r->cur_subvol.received_uuid, uuid, BTRFS_UUID_SIZE);
	r->cur_subvol.stransid = ctransid;

	if (g_verbose) {
		uuid_unparse((u8*)r->cur_subvol.received_uuid, uuid_str);
		fprintf(stderr, "receiving snapshot %s uuid=%s, "
				"ctransid=%llu ", path, uuid_str,
				r->cur_subvol.stransid);
		uuid_unparse(parent_uuid, uuid_str);
		fprintf(stderr, "parent_uuid=%s, parent_ctransid=%llu\n",
				uuid_str, parent_ctransid);
	}

	memset(&args_v2, 0, sizeof(args_v2));
	strncpy_null(args_v2.name, path);

	parent_subvol = subvol_uuid_search(&r->sus, 0, parent_uuid,
			parent_ctransid, NULL, subvol_search_by_received_uuid);
	if (!parent_subvol) {
		parent_subvol = subvol_uuid_search(&r->sus, 0, parent_uuid,
				parent_ctransid, NULL, subvol_search_by_uuid);
	}
	if (!parent_subvol) {
		ret = -ENOENT;
		error("cannot find parent subvolume");
		goto out;
	}

	/*
	 * The path is resolved from the root subvol, but we could be in some
	 * subvolume under the root subvolume, so try and adjust the path to be
	 * relative to our root path.
	 */
	if (r->full_root_path) {
		size_t root_len;
		size_t sub_len;

		root_len = strlen(r->full_root_path);
		sub_len = strlen(parent_subvol->path);

		/* First make sure the parent subvol is actually in our path */
		if (sub_len < root_len ||
		    strstr(parent_subvol->path, r->full_root_path) == NULL) {
			error(
		"parent subvol is not reachable from inside the root subvol");
			ret = -ENOENT;
			goto out;
		}

		if (sub_len == root_len) {
			parent_subvol->path[0] = '/';
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
	/*if (rs_args.ctransid > rs_args.rtransid) {
		if (!r->force) {
			ret = -EINVAL;
			fprintf(stderr, "ERROR: subvolume %s was modified after it was received.\n", r->subvol_parent_name);
			goto out;
		} else {
			fprintf(stderr, "WARNING: subvolume %s was modified after it was received.\n", r->subvol_parent_name);
		}
	}*/

	if (*parent_subvol->path == 0)
		args_v2.fd = dup(r->mnt_fd);
	else
		args_v2.fd = openat(r->mnt_fd, parent_subvol->path,
				O_RDONLY | O_NOATIME);
	if (args_v2.fd < 0) {
		ret = -errno;
		if (errno != ENOENT)
			error("cannot open %s: %s",
					parent_subvol->path, strerror(-ret));
		else
			fprintf(stderr,
				"It seems that you have changed your default "
				"subvolume or you specify other subvolume to\n"
				"mount btrfs, try to remount this btrfs filesystem "
				"with fs tree, and run btrfs receive again!\n");
		goto out;
	}

	ret = ioctl(r->dest_dir_fd, BTRFS_IOC_SNAP_CREATE_V2, &args_v2);
	close(args_v2.fd);
	if (ret < 0) {
		ret = -errno;
		error("creating snapshot %s -> %s failed: %s",
				parent_subvol->path, path, strerror(-ret));
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
	struct btrfs_receive *r = user;
	char full_path[PATH_MAX];

	ret = path_cat_out(full_path, r->full_subvol_path, path);
	if (ret < 0) {
		error("mkfile: path invalid: %s", path);
		goto out;
	}

	if (g_verbose >= 2)
		fprintf(stderr, "mkfile %s\n", path);

	ret = creat(full_path, 0600);
	if (ret < 0) {
		ret = -errno;
		error("mkfile %s failed: %s", path, strerror(-ret));
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
	struct btrfs_receive *r = user;
	char full_path[PATH_MAX];

	ret = path_cat_out(full_path, r->full_subvol_path, path);
	if (ret < 0) {
		error("mkdir: path invalid: %s", path);
		goto out;
	}

	if (g_verbose >= 2)
		fprintf(stderr, "mkdir %s\n", path);

	ret = mkdir(full_path, 0700);
	if (ret < 0) {
		ret = -errno;
		error("mkdir %s failed: %s", path, strerror(-ret));
	}

out:
	return ret;
}

static int process_mknod(const char *path, u64 mode, u64 dev, void *user)
{
	int ret;
	struct btrfs_receive *r = user;
	char full_path[PATH_MAX];

	ret = path_cat_out(full_path, r->full_subvol_path, path);
	if (ret < 0) {
		error("mknod: path invalid: %s", path);
		goto out;
	}

	if (g_verbose >= 2)
		fprintf(stderr, "mknod %s mode=%llu, dev=%llu\n",
				path, mode, dev);

	ret = mknod(full_path, mode & S_IFMT, dev);
	if (ret < 0) {
		ret = -errno;
		error("mknod %s failed: %s", path, strerror(-ret));
	}

out:
	return ret;
}

static int process_mkfifo(const char *path, void *user)
{
	int ret;
	struct btrfs_receive *r = user;
	char full_path[PATH_MAX];

	ret = path_cat_out(full_path, r->full_subvol_path, path);
	if (ret < 0) {
		error("mkfifo: path invalid: %s", path);
		goto out;
	}

	if (g_verbose >= 2)
		fprintf(stderr, "mkfifo %s\n", path);

	ret = mkfifo(full_path, 0600);
	if (ret < 0) {
		ret = -errno;
		error("mkfifo %s failed: %s", path, strerror(-ret));
	}

out:
	return ret;
}

static int process_mksock(const char *path, void *user)
{
	int ret;
	struct btrfs_receive *r = user;
	char full_path[PATH_MAX];

	ret = path_cat_out(full_path, r->full_subvol_path, path);
	if (ret < 0) {
		error("mksock: path invalid: %s", path);
		goto out;
	}

	if (g_verbose >= 2)
		fprintf(stderr, "mksock %s\n", path);

	ret = mknod(full_path, 0600 | S_IFSOCK, 0);
	if (ret < 0) {
		ret = -errno;
		error("mknod %s failed: %s", path, strerror(-ret));
	}

out:
	return ret;
}

static int process_symlink(const char *path, const char *lnk, void *user)
{
	int ret;
	struct btrfs_receive *r = user;
	char full_path[PATH_MAX];

	ret = path_cat_out(full_path, r->full_subvol_path, path);
	if (ret < 0) {
		error("symlink: path invalid: %s", path);
		goto out;
	}

	if (g_verbose >= 2)
		fprintf(stderr, "symlink %s -> %s\n", path, lnk);

	ret = symlink(lnk, full_path);
	if (ret < 0) {
		ret = -errno;
		error("symlink %s -> %s failed: %s", path,
				lnk, strerror(-ret));
	}

out:
	return ret;
}

static int process_rename(const char *from, const char *to, void *user)
{
	int ret;
	struct btrfs_receive *r = user;
	char full_from[PATH_MAX];
	char full_to[PATH_MAX];

	ret = path_cat_out(full_from, r->full_subvol_path, from);
	if (ret < 0) {
		error("rename: source path invalid: %s", from);
		goto out;
	}

	ret = path_cat_out(full_to, r->full_subvol_path, to);
	if (ret < 0) {
		error("rename: target path invalid: %s", to);
		goto out;
	}

	if (g_verbose >= 2)
		fprintf(stderr, "rename %s -> %s\n", from, to);

	ret = rename(full_from, full_to);
	if (ret < 0) {
		ret = -errno;
		error("rename %s -> %s failed: %s", from,
				to, strerror(-ret));
	}

out:
	return ret;
}

static int process_link(const char *path, const char *lnk, void *user)
{
	int ret;
	struct btrfs_receive *r = user;
	char full_path[PATH_MAX];
	char full_link_path[PATH_MAX];

	ret = path_cat_out(full_path, r->full_subvol_path, path);
	if (ret < 0) {
		error("link: source path invalid: %s", full_path);
		goto out;
	}

	ret = path_cat_out(full_link_path, r->full_subvol_path, lnk);
	if (ret < 0) {
		error("link: target path invalid: %s", full_link_path);
		goto out;
	}

	if (g_verbose >= 2)
		fprintf(stderr, "link %s -> %s\n", path, lnk);

	ret = link(full_link_path, full_path);
	if (ret < 0) {
		ret = -errno;
		error("link %s -> %s failed: %s", path, lnk, strerror(-ret));
	}

out:
	return ret;
}


static int process_unlink(const char *path, void *user)
{
	int ret;
	struct btrfs_receive *r = user;
	char full_path[PATH_MAX];

	ret = path_cat_out(full_path, r->full_subvol_path, path);
	if (ret < 0) {
		error("unlink: path invalid: %s", path);
		goto out;
	}

	if (g_verbose >= 2)
		fprintf(stderr, "unlink %s\n", path);

	ret = unlink(full_path);
	if (ret < 0) {
		ret = -errno;
		error("unlink %s failed. %s", path, strerror(-ret));
	}

out:
	return ret;
}

static int process_rmdir(const char *path, void *user)
{
	int ret;
	struct btrfs_receive *r = user;
	char full_path[PATH_MAX];

	ret = path_cat_out(full_path, r->full_subvol_path, path);
	if (ret < 0) {
		error("rmdir: path invalid: %s", path);
		goto out;
	}

	if (g_verbose >= 2)
		fprintf(stderr, "rmdir %s\n", path);

	ret = rmdir(full_path);
	if (ret < 0) {
		ret = -errno;
		error("rmdir %s failed: %s", path, strerror(-ret));
	}

out:
	return ret;
}

static int open_inode_for_write(struct btrfs_receive *r, const char *path)
{
	int ret = 0;

	if (r->write_fd != -1) {
		if (strcmp(r->write_path, path) == 0)
			goto out;
		close(r->write_fd);
		r->write_fd = -1;
	}

	r->write_fd = open(path, O_RDWR);
	if (r->write_fd < 0) {
		ret = -errno;
		error("cannot open %s: %s", path, strerror(-ret));
		goto out;
	}
	strncpy_null(r->write_path, path);

out:
	return ret;
}

static void close_inode_for_write(struct btrfs_receive *r)
{
	if(r->write_fd == -1)
		return;

	close(r->write_fd);
	r->write_fd = -1;
	r->write_path[0] = 0;
}

static int process_write(const char *path, const void *data, u64 offset,
			 u64 len, void *user)
{
	int ret = 0;
	struct btrfs_receive *r = user;
	char full_path[PATH_MAX];
	u64 pos = 0;
	int w;

	ret = path_cat_out(full_path, r->full_subvol_path, path);
	if (ret < 0) {
		error("write: path invalid: %s", path);
		goto out;
	}

	ret = open_inode_for_write(r, full_path);
	if (ret < 0)
		goto out;

	while (pos < len) {
		w = pwrite(r->write_fd, (char*)data + pos, len - pos,
				offset + pos);
		if (w < 0) {
			ret = -errno;
			error("writing to %s failed: %s\n",
					path, strerror(-ret));
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
	struct btrfs_receive *r = user;
	struct btrfs_ioctl_clone_range_args clone_args;
	struct subvol_info *si = NULL;
	char full_path[PATH_MAX];
	char *subvol_path = NULL;
	char full_clone_path[PATH_MAX];
	int clone_fd = -1;

	ret = path_cat_out(full_path, r->full_subvol_path, path);
	if (ret < 0) {
		error("clone: source path invalid: %s", path);
		goto out;
	}

	ret = open_inode_for_write(r, full_path);
	if (ret < 0)
		goto out;

	si = subvol_uuid_search(&r->sus, 0, clone_uuid, clone_ctransid, NULL,
			subvol_search_by_received_uuid);
	if (!si) {
		if (memcmp(clone_uuid, r->cur_subvol.received_uuid,
				BTRFS_UUID_SIZE) == 0) {
			/* TODO check generation of extent */
			subvol_path = strdup(r->cur_subvol_path);
		} else {
			ret = -ENOENT;
			error("clone: did not find source subvol");
			goto out;
		}
	} else {
		/*if (rs_args.ctransid > rs_args.rtransid) {
			if (!r->force) {
				ret = -EINVAL;
				fprintf(stderr, "ERROR: subvolume %s was "
						"modified after it was "
						"received.\n",
						r->subvol_parent_name);
				goto out;
			} else {
				fprintf(stderr, "WARNING: subvolume %s was "
						"modified after it was "
						"received.\n",
						r->subvol_parent_name);
			}
		}*/
		subvol_path = strdup(si->path);
	}

	ret = path_cat_out(full_clone_path, subvol_path, clone_path);
	if (ret < 0) {
		error("clone: target path invalid: %s", clone_path);
		goto out;
	}

	clone_fd = openat(r->mnt_fd, full_clone_path, O_RDONLY | O_NOATIME);
	if (clone_fd < 0) {
		ret = -errno;
		error("cannot open %s: %s", full_clone_path, strerror(-ret));
		goto out;
	}

	clone_args.src_fd = clone_fd;
	clone_args.src_offset = clone_offset;
	clone_args.src_length = len;
	clone_args.dest_offset = offset;
	ret = ioctl(r->write_fd, BTRFS_IOC_CLONE_RANGE, &clone_args);
	if (ret < 0) {
		ret = -errno;
		error("failed to clone extents to %s\n%s\n",
				path, strerror(-ret));
		goto out;
	}

out:
	if (si) {
		free(si->path);
		free(si);
	}
	free(subvol_path);
	if (clone_fd != -1)
		close(clone_fd);
	return ret;
}


static int process_set_xattr(const char *path, const char *name,
			     const void *data, int len, void *user)
{
	int ret = 0;
	struct btrfs_receive *r = user;
	char full_path[PATH_MAX];

	ret = path_cat_out(full_path, r->full_subvol_path, path);
	if (ret < 0) {
		error("set_xattr: path invalid: %s", path);
		goto out;
	}

	if (strcmp("security.capability", name) == 0) {
		if (g_verbose >= 3)
			fprintf(stderr, "set_xattr: cache capabilities\n");
		if (r->cached_capabilities_len)
			warning("capabilities set multiple times per file: %s",
				full_path);
		if (len > sizeof(r->cached_capabilities)) {
			error("capabilities encoded to %d bytes, buffer too small",
				len);
			ret = -E2BIG;
			goto out;
		}
		r->cached_capabilities_len = len;
		memcpy(r->cached_capabilities, data, len);
	}

	if (g_verbose >= 2) {
		fprintf(stderr, "set_xattr %s - name=%s data_len=%d "
				"data=%.*s\n", path, name, len,
				len, (char*)data);
	}

	ret = lsetxattr(full_path, name, data, len, 0);
	if (ret < 0) {
		ret = -errno;
		error("lsetxattr %s %s=%.*s failed: %s",
				path, name, len, (char*)data, strerror(-ret));
		goto out;
	}

out:
	return ret;
}

static int process_remove_xattr(const char *path, const char *name, void *user)
{
	int ret = 0;
	struct btrfs_receive *r = user;
	char full_path[PATH_MAX];

	ret = path_cat_out(full_path, r->full_subvol_path, path);
	if (ret < 0) {
		error("remove_xattr: path invalid: %s", path);
		goto out;
	}

	if (g_verbose >= 2) {
		fprintf(stderr, "remove_xattr %s - name=%s\n",
				path, name);
	}

	ret = lremovexattr(full_path, name);
	if (ret < 0) {
		ret = -errno;
		error("lremovexattr %s %s failed: %s",
				path, name, strerror(-ret));
		goto out;
	}

out:
	return ret;
}

static int process_truncate(const char *path, u64 size, void *user)
{
	int ret = 0;
	struct btrfs_receive *r = user;
	char full_path[PATH_MAX];

	ret = path_cat_out(full_path, r->full_subvol_path, path);
	if (ret < 0) {
		error("truncate: path invalid: %s", path);
		goto out;
	}

	if (g_verbose >= 2)
		fprintf(stderr, "truncate %s size=%llu\n", path, size);

	ret = truncate(full_path, size);
	if (ret < 0) {
		ret = -errno;
		error("truncate %s failed: %s", path, strerror(-ret));
		goto out;
	}

out:
	return ret;
}

static int process_chmod(const char *path, u64 mode, void *user)
{
	int ret = 0;
	struct btrfs_receive *r = user;
	char full_path[PATH_MAX];

	ret = path_cat_out(full_path, r->full_subvol_path, path);
	if (ret < 0) {
		error("chmod: path invalid: %s", path);
		goto out;
	}

	if (g_verbose >= 2)
		fprintf(stderr, "chmod %s - mode=0%o\n", path, (int)mode);

	ret = chmod(full_path, mode);
	if (ret < 0) {
		ret = -errno;
		error("chmod %s failed: %s", path, strerror(-ret));
		goto out;
	}

out:
	return ret;
}

static int process_chown(const char *path, u64 uid, u64 gid, void *user)
{
	int ret = 0;
	struct btrfs_receive *r = user;
	char full_path[PATH_MAX];

	ret = path_cat_out(full_path, r->full_subvol_path, path);
	if (ret < 0) {
		error("chown: path invalid: %s", path);
		goto out;
	}

	if (g_verbose >= 2)
		fprintf(stderr, "chown %s - uid=%llu, gid=%llu\n", path,
				uid, gid);

	ret = lchown(full_path, uid, gid);
	if (ret < 0) {
		ret = -errno;
		error("chown %s failed: %s", path, strerror(-ret));
		goto out;
	}

	if (r->cached_capabilities_len) {
		if (g_verbose >= 2)
			fprintf(stderr, "chown: restore capabilities\n");
		ret = lsetxattr(full_path, "security.capability",
				r->cached_capabilities,
				r->cached_capabilities_len, 0);
		memset(r->cached_capabilities, 0,
				sizeof(r->cached_capabilities));
		r->cached_capabilities_len = 0;
		if (ret < 0) {
			ret = -errno;
			error("restoring capabilities %s: %s",
					path, strerror(-ret));
			goto out;
		}
	}

out:
	return ret;
}

static int process_utimes(const char *path, struct timespec *at,
			  struct timespec *mt, struct timespec *ct,
			  void *user)
{
	int ret = 0;
	struct btrfs_receive *r = user;
	char full_path[PATH_MAX];
	struct timespec tv[2];

	ret = path_cat_out(full_path, r->full_subvol_path, path);
	if (ret < 0) {
		error("utimes: path invalid: %s", path);
		goto out;
	}

	if (g_verbose >= 2)
		fprintf(stderr, "utimes %s\n", path);

	tv[0] = *at;
	tv[1] = *mt;
	ret = utimensat(AT_FDCWD, full_path, tv, AT_SYMLINK_NOFOLLOW);
	if (ret < 0) {
		ret = -errno;
		error("utimes %s failed: %s",
				path, strerror(-ret));
		goto out;
	}

out:
	return ret;
}

static int process_update_extent(const char *path, u64 offset, u64 len,
		void *user)
{
	if (g_verbose >= 2)
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

static int do_receive(struct btrfs_receive *r, const char *tomnt,
		      char *realmnt, int r_fd, u64 max_errors)
{
	u64 subvol_id;
	int ret;
	char *dest_dir_full_path;
	char root_subvol_path[PATH_MAX];
	int end = 0;

	dest_dir_full_path = realpath(tomnt, NULL);
	if (!dest_dir_full_path) {
		ret = -errno;
		error("realpath(%s) failed: %s", tomnt, strerror(-ret));
		goto out;
	}
	r->dest_dir_fd = open(dest_dir_full_path, O_RDONLY | O_NOATIME);
	if (r->dest_dir_fd < 0) {
		ret = -errno;
		error("cannot open destination directory %s: %s",
			dest_dir_full_path, strerror(-ret));
		goto out;
	}

	if (realmnt[0]) {
		r->root_path = realmnt;
	} else {
		ret = find_mount_root(dest_dir_full_path, &r->root_path);
		if (ret < 0) {
			error("failed to determine mount point for %s: %s",
				dest_dir_full_path, strerror(-ret));
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
	r->mnt_fd = open(r->root_path, O_RDONLY | O_NOATIME);
	if (r->mnt_fd < 0) {
		ret = -errno;
		error("cannot open %s: %s", r->root_path, strerror(-ret));
		goto out;
	}

	/*
	 * If we use -m or a default subvol we want to resolve the path to the
	 * subvolume we're sitting in so that we can adjust the paths of any
	 * subvols we want to receive in.
	 */
	ret = btrfs_list_get_path_rootid(r->mnt_fd, &subvol_id);
	if (ret) {
		error("cannot resolve our subvolid: %d",
			ret);
		goto out;
	}

	root_subvol_path[0] = 0;
	ret = btrfs_subvolid_resolve(r->mnt_fd, root_subvol_path,
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
		r->full_root_path = root_subvol_path;

	if (r->dest_dir_chroot) {
		if (chroot(dest_dir_full_path)) {
			ret = -errno;
			error("failed to chroot to %s: %s",
				dest_dir_full_path, strerror(-ret));
			goto out;
		}
		if (chdir("/")) {
			ret = -errno;
			error("failed to chdir to / after chroot: %s",
				strerror(-ret));
			goto out;
		}
		fprintf(stderr, "Chroot to %s\n", dest_dir_full_path);
		r->root_path = strdup("/");
		r->dest_dir_path = r->root_path;
	} else {
		/*
		 * find_mount_root returns a root_path that is a subpath of
		 * dest_dir_full_path. Now get the other part of root_path,
		 * which is the destination dir relative to root_path.
		 */
		r->dest_dir_path = dest_dir_full_path + strlen(r->root_path);
		while (r->dest_dir_path[0] == '/')
			r->dest_dir_path++;
	}

	ret = subvol_uuid_search_init(r->mnt_fd, &r->sus);
	if (ret < 0)
		goto out;

	while (!end) {
		if (r->cached_capabilities_len) {
			if (g_verbose >= 3)
				fprintf(stderr, "clear cached capabilities\n");
			memset(r->cached_capabilities, 0,
					sizeof(r->cached_capabilities));
			r->cached_capabilities_len = 0;
		}

		ret = btrfs_read_and_process_send_stream(r_fd, &send_ops, r,
							 r->honor_end_cmd,
							 max_errors);
		if (ret < 0)
			goto out;
		if (ret)
			end = 1;

		close_inode_for_write(r);
		ret = finish_subvol(r);
		if (ret < 0)
			goto out;
	}
	ret = 0;

out:
	if (r->write_fd != -1) {
		close(r->write_fd);
		r->write_fd = -1;
	}

	if (r->root_path != realmnt)
		free(r->root_path);
	r->root_path = NULL;
	r->dest_dir_path = NULL;
	free(dest_dir_full_path);
	subvol_uuid_search_finit(&r->sus);
	if (r->mnt_fd != -1) {
		close(r->mnt_fd);
		r->mnt_fd = -1;
	}
	if (r->dest_dir_fd != -1) {
		close(r->dest_dir_fd);
		r->dest_dir_fd = -1;
	}

	return ret;
}

int cmd_receive(int argc, char **argv)
{
	char *tomnt = NULL;
	char fromfile[PATH_MAX];
	char realmnt[PATH_MAX];
	struct btrfs_receive r;
	int receive_fd = fileno(stdin);
	u64 max_errors = 1;
	int ret = 0;

	memset(&r, 0, sizeof(r));
	r.mnt_fd = -1;
	r.write_fd = -1;
	r.dest_dir_fd = -1;
	r.dest_dir_chroot = 0;
	realmnt[0] = 0;
	fromfile[0] = 0;

	while (1) {
		int c;
		static const struct option long_opts[] = {
			{ "max-errors", required_argument, NULL, 'E' },
			{ "chroot", no_argument, NULL, 'C' },
			{ NULL, 0, NULL, 0 }
		};

		c = getopt_long(argc, argv, "Cevf:m:", long_opts, NULL);
		if (c < 0)
			break;

		switch (c) {
		case 'v':
			g_verbose++;
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
			r.honor_end_cmd = 1;
			break;
		case 'C':
			r.dest_dir_chroot = 1;
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
		case '?':
		default:
			error("receive args invalid");
			return 1;
		}
	}

	if (check_argc_exact(argc - optind, 1))
		usage(cmd_receive_usage);

	tomnt = argv[optind];

	if (fromfile[0]) {
		receive_fd = open(fromfile, O_RDONLY | O_NOATIME);
		if (receive_fd < 0) {
			error("cannot open %s: %s", fromfile, strerror(errno));
			goto out;
		}
	}

	ret = do_receive(&r, tomnt, realmnt, receive_fd, max_errors);
	if (receive_fd != fileno(stdin))
		close(receive_fd);

out:

	return !!ret;
}

const char * const cmd_receive_usage[] = {
	"btrfs receive [-ve] [-f <infile>] [--max-errors <N>] <mount>",
	"Receive subvolumes from stdin.",
	"Receives one or more subvolumes that were previously",
	"sent with btrfs send. The received subvolumes are stored",
	"into <mount>.",
	"btrfs receive will fail in case a receiving subvolume",
	"already exists. It will also fail in case a previously",
	"received subvolume was changed after it was received.",
	"After receiving a subvolume, it is immediately set to",
	"read only.\n",
	"-v               Enable verbose debug output. Each",
	"                 occurrence of this option increases the",
	"                 verbose level more.",
	"-f <infile>      By default, btrfs receive uses stdin",
	"                 to receive the subvolumes. Use this",
	"                 option to specify a file to use instead.",
	"-e               Terminate after receiving an <end cmd>",
	"                 in the data stream. Without this option,",
	"                 the receiver terminates only if an error",
	"                 is recognized or on EOF.",
	"-C|--chroot      confine the process to <mount> using chroot",
	"--max-errors <N> Terminate as soon as N errors happened while",
	"                 processing commands from the send stream.",
	"                 Default value is 1. A value of 0 means no limit.",
	"-m <mountpoint>  The root mount point of the destination fs.",
	"                 If you do not have /proc use this to tell us where ",
	"                 this file system is mounted.",
	NULL
};
