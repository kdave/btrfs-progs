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

#if BTRFS_FLAT_INCLUDES
#include "kerncompat.h"
#include "version.h"
#include "kernel-lib/rbtree.h"
#include "kernel-lib/radix-tree.h"
#include "crypto/crc32c.h"
#include "kernel-lib/list.h"
#include "kernel-lib/sizes.h"
#include "kernel-shared/ctree.h"
#include "kernel-shared/extent_io.h"
#include "ioctl.h"
#include "common/extent-cache.h"
#include "kernel-shared/send.h"
#include "common/send-stream.h"
#include "common/send-utils.h"
#else
/*
 * This needs to include headers the same way as an external program but must
 * not use the existing system headers, so we use "...".
 */
#include "btrfs/kerncompat.h"
#include "btrfs/version.h"
#include "btrfs/rbtree.h"
#include "btrfs/radix-tree.h"
#include "btrfs/crc32c.h"
#include "btrfs/list.h"
#include "btrfs/sizes.h"
#include "btrfs/ctree.h"
#include "btrfs/extent_io.h"
#include "btrfs/ioctl.h"
#include "btrfs/extent-cache.h"
#include "btrfs/send.h"
#include "btrfs/send-stream.h"
#include "btrfs/send-utils.h"
#endif

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
