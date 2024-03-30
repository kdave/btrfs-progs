/*
 * Copyright (C) 2014 SUSE.  All rights reserved.
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

/*
 * This program is only linked to libbtrfsutil library, and only include
 * headers from libbtrfsutil, so we do not use the filepath inside btrfs-progs
 * source code.
 */
#include "btrfs/kerncompat.h"
#include "btrfs/version.h"
#include "btrfs/send-stream.h"
#include "btrfs/send-utils.h"

/*
 * Reduced code snippet from snapper.git/snapper/Btrfs.cc
 */
struct btrfs_send_ops send_ops = {
	.subvol = NULL,
	.snapshot = NULL,
	.mkfile = NULL,
	.mkdir = NULL,
	.mknod = NULL,
	.mkfifo = NULL,
	.mksock = NULL,
	.symlink = NULL,
	.rename = NULL,
	.link = NULL,
	.unlink = NULL,
	.rmdir = NULL,
	.write = NULL,
	.clone = NULL,
	.set_xattr = NULL,
	.remove_xattr = NULL,
	.truncate = NULL,
	.chmod = NULL,
	.chown = NULL,
	.utimes = NULL,
	.update_extent = NULL,
};

/*
 * Link test only, not intended to be executed.
 */
static int test_send_stream_api() {
	int ret;
	int fd = -1;

#if BTRFS_LIB_VERSION < 101
	ret = btrfs_read_and_process_send_stream(fd, &send_ops, NULL, 0);
#else
	ret = btrfs_read_and_process_send_stream(fd, &send_ops, NULL, 0, 1);
#endif
	return ret;
}

static int test_uuid_search() {
	struct subvol_uuid_search sus = {};
	u8 uuid[BTRFS_FSID_SIZE] = {};

	subvol_uuid_search_init(-1, &sus);
	subvol_uuid_search(&sus, 0, uuid, -1, "/", subvol_search_by_path);
	return 0;
}

static int test_subvolid_resolve() {
	char path[4096] = "/";

	btrfs_subvolid_resolve(-1, path, strlen(path), 0);
	return 0;
}

int main() {
	test_send_stream_api();
	test_uuid_search();
	test_subvolid_resolve();

	return 0;
}
