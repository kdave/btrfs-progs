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

#define _GNU_SOURCE
#define _POSIX_C_SOURCE 200809
#define _XOPEN_SOURCE 700
#define _BSD_SOURCE

#include "kerncompat.h"

#include <unistd.h>
#include <stdint.h>
#include <dirent.h>
#include <fcntl.h>
#include <pthread.h>
#include <math.h>
#include <ftw.h>
#include <wait.h>
#include <assert.h>

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
	char *write_path;

	char *root_path;
	char *dest_dir_path; /* relative to root_path */
	char *full_subvol_path;

	struct subvol_info *cur_subvol;

	struct subvol_uuid_search sus;

	int honor_end_cmd;
};

static int finish_subvol(struct btrfs_receive *r)
{
	int ret;
	int subvol_fd = -1;
	struct btrfs_ioctl_received_subvol_args rs_args;
	char uuid_str[128];
	u64 flags;

	if (r->cur_subvol == NULL)
		return 0;

	subvol_fd = openat(r->mnt_fd, r->cur_subvol->path,
			O_RDONLY | O_NOATIME);
	if (subvol_fd < 0) {
		ret = -errno;
		fprintf(stderr, "ERROR: open %s failed. %s\n",
				r->cur_subvol->path, strerror(-ret));
		goto out;
	}

	memset(&rs_args, 0, sizeof(rs_args));
	memcpy(rs_args.uuid, r->cur_subvol->received_uuid, BTRFS_UUID_SIZE);
	rs_args.stransid = r->cur_subvol->stransid;

	if (g_verbose >= 1) {
		uuid_unparse((u8*)rs_args.uuid, uuid_str);
		fprintf(stderr, "BTRFS_IOC_SET_RECEIVED_SUBVOL uuid=%s, "
				"stransid=%llu\n", uuid_str, rs_args.stransid);
	}

	ret = ioctl(subvol_fd, BTRFS_IOC_SET_RECEIVED_SUBVOL, &rs_args);
	if (ret < 0) {
		ret = -errno;
		fprintf(stderr, "ERROR: BTRFS_IOC_SET_RECEIVED_SUBVOL failed. %s\n",
				strerror(-ret));
		goto out;
	}
	r->cur_subvol->rtransid = rs_args.rtransid;

	ret = ioctl(subvol_fd, BTRFS_IOC_SUBVOL_GETFLAGS, &flags);
	if (ret < 0) {
		ret = -errno;
		fprintf(stderr, "ERROR: BTRFS_IOC_SUBVOL_GETFLAGS failed. %s\n",
				strerror(-ret));
		goto out;
	}

	flags |= BTRFS_SUBVOL_RDONLY;

	ret = ioctl(subvol_fd, BTRFS_IOC_SUBVOL_SETFLAGS, &flags);
	if (ret < 0) {
		ret = -errno;
		fprintf(stderr, "ERROR: failed to make subvolume read only. "
				"%s\n", strerror(-ret));
		goto out;
	}

	ret = 0;

out:
	if (r->cur_subvol) {
		free(r->cur_subvol->path);
		free(r->cur_subvol);
		r->cur_subvol = NULL;
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
	char uuid_str[128];

	ret = finish_subvol(r);
	if (ret < 0)
		goto out;

	r->cur_subvol = calloc(1, sizeof(*r->cur_subvol));

	if (strlen(r->dest_dir_path) == 0)
		r->cur_subvol->path = strdup(path);
	else
		r->cur_subvol->path = path_cat(r->dest_dir_path, path);
	free(r->full_subvol_path);
	r->full_subvol_path = path_cat3(r->root_path, r->dest_dir_path, path);

	fprintf(stderr, "At subvol %s\n", path);

	memcpy(r->cur_subvol->received_uuid, uuid, BTRFS_UUID_SIZE);
	r->cur_subvol->stransid = ctransid;

	if (g_verbose) {
		uuid_unparse((u8*)r->cur_subvol->received_uuid, uuid_str);
		fprintf(stderr, "receiving subvol %s uuid=%s, stransid=%llu\n",
				path, uuid_str,
				r->cur_subvol->stransid);
	}

	memset(&args_v1, 0, sizeof(args_v1));
	strncpy_null(args_v1.name, path);
	ret = ioctl(r->dest_dir_fd, BTRFS_IOC_SUBVOL_CREATE, &args_v1);
	if (ret < 0) {
		ret = -errno;
		fprintf(stderr, "ERROR: creating subvolume %s failed. "
				"%s\n", path, strerror(-ret));
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
	char uuid_str[128];
	struct btrfs_ioctl_vol_args_v2 args_v2;
	struct subvol_info *parent_subvol = NULL;

	ret = finish_subvol(r);
	if (ret < 0)
		goto out;

	r->cur_subvol = calloc(1, sizeof(*r->cur_subvol));

	if (strlen(r->dest_dir_path) == 0)
		r->cur_subvol->path = strdup(path);
	else
		r->cur_subvol->path = path_cat(r->dest_dir_path, path);
	free(r->full_subvol_path);
	r->full_subvol_path = path_cat3(r->root_path, r->dest_dir_path, path);

	fprintf(stderr, "At snapshot %s\n", path);

	memcpy(r->cur_subvol->received_uuid, uuid, BTRFS_UUID_SIZE);
	r->cur_subvol->stransid = ctransid;

	if (g_verbose) {
		uuid_unparse((u8*)r->cur_subvol->received_uuid, uuid_str);
		fprintf(stderr, "receiving snapshot %s uuid=%s, "
				"ctransid=%llu ", path, uuid_str,
				r->cur_subvol->stransid);
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
		fprintf(stderr, "ERROR: could not find parent subvolume\n");
		goto out;
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

	args_v2.fd = openat(r->mnt_fd, parent_subvol->path,
			O_RDONLY | O_NOATIME);
	if (args_v2.fd < 0) {
		ret = -errno;
		fprintf(stderr, "ERROR: open %s failed. %s\n",
				parent_subvol->path, strerror(-ret));
		goto out;
	}

	ret = ioctl(r->dest_dir_fd, BTRFS_IOC_SNAP_CREATE_V2, &args_v2);
	close(args_v2.fd);
	if (ret < 0) {
		ret = -errno;
		fprintf(stderr, "ERROR: creating snapshot %s -> %s "
				"failed. %s\n", parent_subvol->path,
				path, strerror(-ret));
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
	char *full_path = path_cat(r->full_subvol_path, path);

	if (g_verbose >= 2)
		fprintf(stderr, "mkfile %s\n", path);

	ret = creat(full_path, 0600);
	if (ret < 0) {
		ret = -errno;
		fprintf(stderr, "ERROR: mkfile %s failed. %s\n", path,
				strerror(-ret));
		goto out;
	}
	close(ret);
	ret = 0;

out:
	free(full_path);
	return ret;
}

static int process_mkdir(const char *path, void *user)
{
	int ret;
	struct btrfs_receive *r = user;
	char *full_path = path_cat(r->full_subvol_path, path);

	if (g_verbose >= 2)
		fprintf(stderr, "mkdir %s\n", path);

	ret = mkdir(full_path, 0700);
	if (ret < 0) {
		ret = -errno;
		fprintf(stderr, "ERROR: mkdir %s failed. %s\n", path,
				strerror(-ret));
	}

	free(full_path);
	return ret;
}

static int process_mknod(const char *path, u64 mode, u64 dev, void *user)
{
	int ret;
	struct btrfs_receive *r = user;
	char *full_path = path_cat(r->full_subvol_path, path);

	if (g_verbose >= 2)
		fprintf(stderr, "mknod %s mode=%llu, dev=%llu\n",
				path, mode, dev);

	ret = mknod(full_path, mode & S_IFMT, dev);
	if (ret < 0) {
		ret = -errno;
		fprintf(stderr, "ERROR: mknod %s failed. %s\n", path,
				strerror(-ret));
	}

	free(full_path);
	return ret;
}

static int process_mkfifo(const char *path, void *user)
{
	int ret;
	struct btrfs_receive *r = user;
	char *full_path = path_cat(r->full_subvol_path, path);

	if (g_verbose >= 2)
		fprintf(stderr, "mkfifo %s\n", path);

	ret = mkfifo(full_path, 0600);
	if (ret < 0) {
		ret = -errno;
		fprintf(stderr, "ERROR: mkfifo %s failed. %s\n", path,
				strerror(-ret));
	}

	free(full_path);
	return ret;
}

static int process_mksock(const char *path, void *user)
{
	int ret;
	struct btrfs_receive *r = user;
	char *full_path = path_cat(r->full_subvol_path, path);

	if (g_verbose >= 2)
		fprintf(stderr, "mksock %s\n", path);

	ret = mknod(full_path, 0600 | S_IFSOCK, 0);
	if (ret < 0) {
		ret = -errno;
		fprintf(stderr, "ERROR: mknod %s failed. %s\n", path,
				strerror(-ret));
	}

	free(full_path);
	return ret;
}

static int process_symlink(const char *path, const char *lnk, void *user)
{
	int ret;
	struct btrfs_receive *r = user;
	char *full_path = path_cat(r->full_subvol_path, path);

	if (g_verbose >= 2)
		fprintf(stderr, "symlink %s -> %s\n", path, lnk);

	ret = symlink(lnk, full_path);
	if (ret < 0) {
		ret = -errno;
		fprintf(stderr, "ERROR: symlink %s -> %s failed. %s\n", path,
				lnk, strerror(-ret));
	}

	free(full_path);
	return ret;
}

static int process_rename(const char *from, const char *to, void *user)
{
	int ret;
	struct btrfs_receive *r = user;
	char *full_from = path_cat(r->full_subvol_path, from);
	char *full_to = path_cat(r->full_subvol_path, to);

	if (g_verbose >= 2)
		fprintf(stderr, "rename %s -> %s\n", from, to);

	ret = rename(full_from, full_to);
	if (ret < 0) {
		ret = -errno;
		fprintf(stderr, "ERROR: rename %s -> %s failed. %s\n", from,
				to, strerror(-ret));
	}

	free(full_from);
	free(full_to);
	return ret;
}

static int process_link(const char *path, const char *lnk, void *user)
{
	int ret;
	struct btrfs_receive *r = user;
	char *full_path = path_cat(r->full_subvol_path, path);
	char *full_link_path = path_cat(r->full_subvol_path, lnk);

	if (g_verbose >= 2)
		fprintf(stderr, "link %s -> %s\n", path, lnk);

	ret = link(full_link_path, full_path);
	if (ret < 0) {
		ret = -errno;
		fprintf(stderr, "ERROR: link %s -> %s failed. %s\n", path,
				lnk, strerror(-ret));
	}

	free(full_path);
	free(full_link_path);
	return ret;
}


static int process_unlink(const char *path, void *user)
{
	int ret;
	struct btrfs_receive *r = user;
	char *full_path = path_cat(r->full_subvol_path, path);

	if (g_verbose >= 2)
		fprintf(stderr, "unlink %s\n", path);

	ret = unlink(full_path);
	if (ret < 0) {
		ret = -errno;
		fprintf(stderr, "ERROR: unlink %s failed. %s\n", path,
				strerror(-ret));
	}

	free(full_path);
	return ret;
}

static int process_rmdir(const char *path, void *user)
{
	int ret;
	struct btrfs_receive *r = user;
	char *full_path = path_cat(r->full_subvol_path, path);

	if (g_verbose >= 2)
		fprintf(stderr, "rmdir %s\n", path);

	ret = rmdir(full_path);
	if (ret < 0) {
		ret = -errno;
		fprintf(stderr, "ERROR: rmdir %s failed. %s\n", path,
				strerror(-ret));
	}

	free(full_path);
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
		fprintf(stderr, "ERROR: open %s failed. %s\n", path,
				strerror(-ret));
		goto out;
	}
	free(r->write_path);
	r->write_path = strdup(path);

out:
	return ret;
}

static int close_inode_for_write(struct btrfs_receive *r)
{
	int ret = 0;

	if(r->write_fd == -1)
		goto out;

	close(r->write_fd);
	r->write_fd = -1;
	r->write_path[0] = 0;

out:
	return ret;
}

static int process_write(const char *path, const void *data, u64 offset,
			 u64 len, void *user)
{
	int ret = 0;
	struct btrfs_receive *r = user;
	char *full_path = path_cat(r->full_subvol_path, path);
	u64 pos = 0;
	int w;

	ret = open_inode_for_write(r, full_path);
	if (ret < 0)
		goto out;

	while (pos < len) {
		w = pwrite(r->write_fd, (char*)data + pos, len - pos,
				offset + pos);
		if (w < 0) {
			ret = -errno;
			fprintf(stderr, "ERROR: writing to %s failed. %s\n",
					path, strerror(-ret));
			goto out;
		}
		pos += w;
	}

out:
	free(full_path);
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
	char *full_path = path_cat(r->full_subvol_path, path);
	char *subvol_path = NULL;
	char *full_clone_path = NULL;
	int clone_fd = -1;

	ret = open_inode_for_write(r, full_path);
	if (ret < 0)
		goto out;

	si = subvol_uuid_search(&r->sus, 0, clone_uuid, clone_ctransid, NULL,
			subvol_search_by_received_uuid);
	if (!si) {
		if (memcmp(clone_uuid, r->cur_subvol->received_uuid,
				BTRFS_UUID_SIZE) == 0) {
			/* TODO check generation of extent */
			subvol_path = strdup(r->cur_subvol->path);
		} else {
			ret = -ENOENT;
			fprintf(stderr, "ERROR: did not find source subvol.\n");
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

	full_clone_path = path_cat3(r->root_path, subvol_path, clone_path);

	clone_fd = open(full_clone_path, O_RDONLY | O_NOATIME);
	if (clone_fd < 0) {
		ret = -errno;
		fprintf(stderr, "ERROR: failed to open %s. %s\n",
				full_clone_path, strerror(-ret));
		goto out;
	}

	clone_args.src_fd = clone_fd;
	clone_args.src_offset = clone_offset;
	clone_args.src_length = len;
	clone_args.dest_offset = offset;
	ret = ioctl(r->write_fd, BTRFS_IOC_CLONE_RANGE, &clone_args);
	if (ret) {
		ret = -errno;
		fprintf(stderr, "ERROR: failed to clone extents to %s\n%s\n",
				path, strerror(-ret));
		goto out;
	}

out:
	if (si) {
		free(si->path);
		free(si);
	}
	free(full_path);
	free(full_clone_path);
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
	char *full_path = path_cat(r->full_subvol_path, path);

	if (g_verbose >= 2) {
		fprintf(stderr, "set_xattr %s - name=%s data_len=%d "
				"data=%.*s\n", path, name, len,
				len, (char*)data);
	}

	ret = lsetxattr(full_path, name, data, len, 0);
	if (ret < 0) {
		ret = -errno;
		fprintf(stderr, "ERROR: lsetxattr %s %s=%.*s failed. %s\n",
				path, name, len, (char*)data, strerror(-ret));
		goto out;
	}

out:
	free(full_path);
	return ret;
}

static int process_remove_xattr(const char *path, const char *name, void *user)
{
	int ret = 0;
	struct btrfs_receive *r = user;
	char *full_path = path_cat(r->full_subvol_path, path);

	if (g_verbose >= 2) {
		fprintf(stderr, "remove_xattr %s - name=%s\n",
				path, name);
	}

	ret = lremovexattr(full_path, name);
	if (ret < 0) {
		ret = -errno;
		fprintf(stderr, "ERROR: lremovexattr %s %s failed. %s\n",
				path, name, strerror(-ret));
		goto out;
	}

out:
	free(full_path);
	return ret;
}

static int process_truncate(const char *path, u64 size, void *user)
{
	int ret = 0;
	struct btrfs_receive *r = user;
	char *full_path = path_cat(r->full_subvol_path, path);

	if (g_verbose >= 2)
		fprintf(stderr, "truncate %s size=%llu\n", path, size);

	ret = truncate(full_path, size);
	if (ret < 0) {
		ret = -errno;
		fprintf(stderr, "ERROR: truncate %s failed. %s\n",
				path, strerror(-ret));
		goto out;
	}

out:
	free(full_path);
	return ret;
}

static int process_chmod(const char *path, u64 mode, void *user)
{
	int ret = 0;
	struct btrfs_receive *r = user;
	char *full_path = path_cat(r->full_subvol_path, path);

	if (g_verbose >= 2)
		fprintf(stderr, "chmod %s - mode=0%o\n", path, (int)mode);

	ret = chmod(full_path, mode);
	if (ret < 0) {
		ret = -errno;
		fprintf(stderr, "ERROR: chmod %s failed. %s\n",
				path, strerror(-ret));
		goto out;
	}

out:
	free(full_path);
	return ret;
}

static int process_chown(const char *path, u64 uid, u64 gid, void *user)
{
	int ret = 0;
	struct btrfs_receive *r = user;
	char *full_path = path_cat(r->full_subvol_path, path);

	if (g_verbose >= 2)
		fprintf(stderr, "chown %s - uid=%llu, gid=%llu\n", path,
				uid, gid);

	ret = lchown(full_path, uid, gid);
	if (ret < 0) {
		ret = -errno;
		fprintf(stderr, "ERROR: chown %s failed. %s\n",
				path, strerror(-ret));
		goto out;
	}

out:
	free(full_path);
	return ret;
}

static int process_utimes(const char *path, struct timespec *at,
			  struct timespec *mt, struct timespec *ct,
			  void *user)
{
	int ret = 0;
	struct btrfs_receive *r = user;
	char *full_path = path_cat(r->full_subvol_path, path);
	struct timespec tv[2];

	if (g_verbose >= 2)
		fprintf(stderr, "utimes %s\n", path);

	tv[0] = *at;
	tv[1] = *mt;
	ret = utimensat(AT_FDCWD, full_path, tv, AT_SYMLINK_NOFOLLOW);
	if (ret < 0) {
		ret = -errno;
		fprintf(stderr, "ERROR: utimes %s failed. %s\n",
				path, strerror(-ret));
		goto out;
	}

out:
	free(full_path);
	return ret;
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
};

static int do_receive(struct btrfs_receive *r, const char *tomnt, int r_fd)
{
	int ret;
	char *dest_dir_full_path;
	int end = 0;

	dest_dir_full_path = realpath(tomnt, NULL);
	if (!dest_dir_full_path) {
		ret = -errno;
		fprintf(stderr, "ERROR: realpath(%s) failed. %s\n", tomnt,
			strerror(-ret));
		goto out;
	}
	r->dest_dir_fd = open(dest_dir_full_path, O_RDONLY | O_NOATIME);
	if (r->dest_dir_fd < 0) {
		ret = -errno;
		fprintf(stderr,
			"ERROR: failed to open destination directory %s. %s\n",
			dest_dir_full_path, strerror(-ret));
		goto out;
	}

	ret = find_mount_root(dest_dir_full_path, &r->root_path);
	if (ret < 0) {
		ret = -EINVAL;
		fprintf(stderr, "ERROR: failed to determine mount point "
			"for %s\n", dest_dir_full_path);
		goto out;
	}
	r->mnt_fd = open(r->root_path, O_RDONLY | O_NOATIME);
	if (r->mnt_fd < 0) {
		ret = -errno;
		fprintf(stderr, "ERROR: failed to open %s. %s\n", r->root_path,
			strerror(-ret));
		goto out;
	}

	/*
	 * find_mount_root returns a root_path that is a subpath of
	 * dest_dir_full_path. Now get the other part of root_path,
	 * which is the destination dir relative to root_path.
	 */
	r->dest_dir_path = dest_dir_full_path + strlen(r->root_path);
	while (r->dest_dir_path[0] == '/')
		r->dest_dir_path++;

	ret = subvol_uuid_search_init(r->mnt_fd, &r->sus);
	if (ret < 0)
		goto out;

	while (!end) {
		ret = btrfs_read_and_process_send_stream(r_fd, &send_ops, r,
							 r->honor_end_cmd);
		if (ret < 0)
			goto out;
		if (ret)
			end = 1;

		ret = close_inode_for_write(r);
		if (ret < 0)
			goto out;
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
	free(r->root_path);
	r->root_path = NULL;
	free(r->write_path);
	r->write_path = NULL;
	free(r->full_subvol_path);
	r->full_subvol_path = NULL;
	r->dest_dir_path = NULL;
	free(dest_dir_full_path);
	if (r->cur_subvol) {
		free(r->cur_subvol->path);
		free(r->cur_subvol);
		r->cur_subvol = NULL;
	}
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
	int c;
	char *tomnt = NULL;
	char *fromfile = NULL;
	struct btrfs_receive r;
	int receive_fd = fileno(stdin);

	int ret;

	memset(&r, 0, sizeof(r));
	r.mnt_fd = -1;
	r.write_fd = -1;
	r.dest_dir_fd = -1;

	while ((c = getopt(argc, argv, "evf:")) != -1) {
		switch (c) {
		case 'v':
			g_verbose++;
			break;
		case 'f':
			fromfile = optarg;
			break;
		case 'e':
			r.honor_end_cmd = 1;
			break;
		case '?':
		default:
			fprintf(stderr, "ERROR: receive args invalid.\n");
			return 1;
		}
	}

	if (optind + 1 != argc) {
		fprintf(stderr, "ERROR: receive needs path to subvolume\n");
		return 1;
	}

	tomnt = argv[optind];

	if (fromfile) {
		receive_fd = open(fromfile, O_RDONLY | O_NOATIME);
		if (receive_fd < 0) {
			fprintf(stderr, "ERROR: failed to open %s\n", fromfile);
			return 1;
		}
	}

	ret = do_receive(&r, tomnt, receive_fd);

	return !!ret;
}

const char * const cmd_receive_usage[] = {
	"btrfs receive [-ve] [-f <infile>] <mount>",
	"Receive subvolumes from stdin.",
	"Receives one or more subvolumes that were previously ",
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
	NULL
};
