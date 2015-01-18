/*
 * Copyright (C) 2013 SUSE.  All rights reserved.
 *
 * This code is adapted from cmds-send.c and cmds-receive.c,
 * Both of which are:
 *
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

#include <unistd.h>
#include <stdint.h>
#include <dirent.h>
#include <pthread.h>
#include <math.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <sys/ioctl.h>
#include <libgen.h>
#include <mntent.h>
#include <limits.h>
#include <stdlib.h>
#include <asm/types.h>
#include <uuid/uuid.h>

/*
 * This should be compilable without the rest of the btrfs-progs
 * source distribution.
 */
#if BTRFS_FLAT_INCLUDES
#include "send-utils.h"
#include "send-stream.h"
#else
#include <btrfs/send-utils.h>
#include <btrfs/send-stream.h>
#endif /* BTRFS_FLAT_INCLUDES */

static int pipefd[2];
struct btrfs_ioctl_send_args io_send = {0, };
static char *subvol_path;
static char *root_path;

struct recv_args {
	char *full_subvol_path;
	char *root_path;
};

void usage(int error)
{
	printf("send-test <btrfs root> <subvol>\n");
	if (error)
		exit(error);
}

static int print_subvol(const char *path, const u8 *uuid, u64 ctransid,
			void *user)
{
	struct recv_args *r = user;
	char uuid_str[BTRFS_UUID_UNPARSED_SIZE];

	r->full_subvol_path = path_cat(r->root_path, path);
	uuid_unparse(uuid, uuid_str);

	printf("subvol\t%s\t%llu\t%s\n", uuid_str,
	       (unsigned long long)ctransid, r->full_subvol_path);

	return 0;
}

static int print_snapshot(const char *path, const u8 *uuid, u64 ctransid,
			  const u8 *parent_uuid, u64 parent_ctransid,
			  void *user)
{
	struct recv_args *r = user;
	char uuid_str[BTRFS_UUID_UNPARSED_SIZE];
	char parent_uuid_str[BTRFS_UUID_UNPARSED_SIZE];

	r->full_subvol_path = path_cat(r->root_path, path);
	uuid_unparse(uuid, uuid_str);
	uuid_unparse(parent_uuid, parent_uuid_str);

	printf("snapshot\t%s\t%llu\t%s\t%llu\t%s\n", uuid_str,
	       (unsigned long long)ctransid, parent_uuid_str,
	       (unsigned long long)parent_ctransid, r->full_subvol_path);

	return 0;
}

static int print_mkfile(const char *path, void *user)
{
	struct recv_args *r = user;
	char *full_path = path_cat(r->full_subvol_path, path);

	printf("mkfile\t%s\n", full_path);

	free(full_path);
	return 0;
}

static int print_mkdir(const char *path, void *user)
{
	struct recv_args *r = user;
	char *full_path = path_cat(r->full_subvol_path, path);

	printf("mkdir\t%s\n", full_path);

	free(full_path);
	return 0;
}

static int print_mknod(const char *path, u64 mode, u64 dev, void *user)
{
	struct recv_args *r = user;
	char *full_path = path_cat(r->full_subvol_path, path);

	printf("mknod\t%llo\t0x%llx\t%s\n", (unsigned long long)mode,
	       (unsigned long long)dev, full_path);

	free(full_path);
	return 0;
}

static int print_mkfifo(const char *path, void *user)
{
	struct recv_args *r = user;
	char *full_path = path_cat(r->full_subvol_path, path);

	printf("mkfifo\t%s\n", full_path);

	free(full_path);
	return 0;
}

static int print_mksock(const char *path, void *user)
{
	struct recv_args *r = user;
	char *full_path = path_cat(r->full_subvol_path, path);

	printf("mksock\t%s\n", full_path);

	free(full_path);
	return 0;
}

static int print_symlink(const char *path, const char *lnk, void *user)
{
	struct recv_args *r = user;
	char *full_path = path_cat(r->full_subvol_path, path);

	printf("symlink\t%s\t%s\n", lnk, full_path);

	free(full_path);
	return 0;
}

static int print_rename(const char *from, const char *to, void *user)
{
	struct recv_args *r = user;
	char *full_from = path_cat(r->full_subvol_path, from);
	char *full_to = path_cat(r->full_subvol_path, to);

	printf("rename\t%s\t%s\n", from, to);

	free(full_from);
	free(full_to);
	return 0;
}

static int print_link(const char *path, const char *lnk, void *user)
{
	struct recv_args *r = user;
	char *full_path = path_cat(r->full_subvol_path, path);

	printf("link\t%s\t%s\n", lnk, full_path);

	free(full_path);
	return 0;
}

static int print_unlink(const char *path, void *user)
{
	struct recv_args *r = user;
	char *full_path = path_cat(r->full_subvol_path, path);

	printf("unlink\t%s\n", full_path);

	free(full_path);
	return 0;
}

static int print_rmdir(const char *path, void *user)
{
	struct recv_args *r = user;
	char *full_path = path_cat(r->full_subvol_path, path);

	printf("rmdir\t%s\n", full_path);

	free(full_path);
	return 0;
}

static int print_write(const char *path, const void *data, u64 offset,
		       u64 len, void *user)
{
	struct recv_args *r = user;
	char *full_path = path_cat(r->full_subvol_path, path);

	printf("write\t%llu\t%llu\t%s\n", (unsigned long long)offset,
	       (unsigned long long)len, full_path);

	free(full_path);
	return 0;
}

static int print_clone(const char *path, u64 offset, u64 len,
		       const u8 *clone_uuid, u64 clone_ctransid,
		       const char *clone_path, u64 clone_offset,
		       void *user)
{
	struct recv_args *r = user;
	char *full_path = path_cat(r->full_subvol_path, path);

	printf("clone\t%s\t%s\n", full_path, clone_path);

	free(full_path);
	return 0;
}

static int print_set_xattr(const char *path, const char *name,
			   const void *data, int len, void *user)
{
	struct recv_args *r = user;
	char *full_path = path_cat(r->full_subvol_path, path);

	printf("set_xattr\t%s\t%s\t%d\n", full_path,
	       name, len);

	free(full_path);
	return 0;
}

static int print_remove_xattr(const char *path, const char *name, void *user)
{
	struct recv_args *r = user;
	char *full_path = path_cat(r->full_subvol_path, path);

	printf("remove_xattr\t%s\t%s\n", full_path, name);

	free(full_path);
	return 0;
}

static int print_truncate(const char *path, u64 size, void *user)
{
	struct recv_args *r = user;
	char *full_path = path_cat(r->full_subvol_path, path);

	printf("truncate\t%llu\t%s\n", (unsigned long long)size, full_path);

	free(full_path);
	return 0;
}

static int print_chmod(const char *path, u64 mode, void *user)
{
	struct recv_args *r = user;
	char *full_path = path_cat(r->full_subvol_path, path);

	printf("chmod\t%llo\t%s\n", (unsigned long long)mode, full_path);

	free(full_path);
	return 0;
}

static int print_chown(const char *path, u64 uid, u64 gid, void *user)
{
	struct recv_args *r = user;
	char *full_path = path_cat(r->full_subvol_path, path);

	printf("chown\t%llu\t%llu\t%s\n", (unsigned long long)uid,
	       (unsigned long long)gid, full_path);

	free(full_path);
	return 0;
}

static int print_utimes(const char *path, struct timespec *at,
			struct timespec *mt, struct timespec *ct,
			void *user)
{
	struct recv_args *r = user;
	char *full_path = path_cat(r->full_subvol_path, path);

	printf("utimes\t%s\n", full_path);

	free(full_path);
	return 0;
}

static int print_update_extent(const char *path, u64 offset, u64 len,
			       void *user)
{
	struct recv_args *r = user;
	char *full_path = path_cat(r->full_subvol_path, path);

	printf("update_extent\t%s\t%llu\t%llu\n", full_path, offset, len);

	free(full_path);
	return 0;
}

static struct btrfs_send_ops send_ops_print = {
	.subvol = print_subvol,
	.snapshot = print_snapshot,
	.mkfile = print_mkfile,
	.mkdir = print_mkdir,
	.mknod = print_mknod,
	.mkfifo = print_mkfifo,
	.mksock = print_mksock,
	.symlink = print_symlink,
	.rename = print_rename,
	.link = print_link,
	.unlink = print_unlink,
	.rmdir = print_rmdir,
	.write = print_write,
	.clone = print_clone,
	.set_xattr = print_set_xattr,
	.remove_xattr = print_remove_xattr,
	.truncate = print_truncate,
	.chmod = print_chmod,
	.chown = print_chown,
	.utimes = print_utimes,
	.update_extent = print_update_extent,
};

static void *process_thread(void *arg_)
{
	int ret;

	while (1) {
		ret = btrfs_read_and_process_send_stream(pipefd[0],
							 &send_ops_print, arg_, 0);
		if (ret)
			break;
	}

	if (ret > 0)
		ret = 0;

	return ERR_PTR(ret);
}

int main(int argc, char **argv)
{
	int ret = 0;
	int subvol_fd;
	pthread_t t_read;
	void *t_err = NULL;
	struct recv_args r;

	if (argc != 3)
		usage(EINVAL);

	root_path = realpath(argv[1], NULL);
	if (!root_path) {
		ret = errno;
		usage(ret);
	}

	subvol_path = realpath(argv[2], NULL);
	if (!subvol_path) {
		ret = errno;
		usage(ret);
	}

	r.full_subvol_path = subvol_path;
	r.root_path = root_path;

	subvol_fd = open(subvol_path, O_RDONLY|O_NOATIME);
	if (subvol_fd < 0) {
		ret = errno;
		fprintf(stderr, "ERROR: Subvolume open failed. %s\n",
			strerror(ret));
		goto out;
	}

	ret = pipe(pipefd);
	if (ret < 0) {
		ret = errno;
		fprintf(stderr, "ERROR: pipe failed. %s\n", strerror(ret));
		goto out;
	}

	ret = pthread_create(&t_read, NULL, process_thread, &r);
	if (ret < 0) {
		ret = errno;
		fprintf(stderr, "ERROR: pthread create failed. %s\n",
			strerror(ret));
		goto out;
	}

	io_send.send_fd = pipefd[1];
	io_send.clone_sources_count = 0;
	io_send.clone_sources = NULL;
	io_send.parent_root = 0;
	io_send.flags = BTRFS_SEND_FLAG_NO_FILE_DATA;

	ret = ioctl(subvol_fd, BTRFS_IOC_SEND, &io_send);
	if (ret) {
		ret = errno;
		fprintf(stderr, "ERROR: send ioctl failed with %d: %s\n", ret,
			strerror(ret));
		goto out;
	}

	close(pipefd[1]);

	ret = pthread_join(t_read, &t_err);
	if (ret) {
		fprintf(stderr, "ERROR: pthread_join failed: %s\n",
			strerror(ret));
		goto out;
	}
	if (t_err) {
		ret = (long int)t_err;
		fprintf(stderr, "ERROR: failed to process send stream, ret=%ld "
			"(%s)\n", (long int)t_err, strerror(ret));
		goto out;
	}

out:
	return !!ret;
}
