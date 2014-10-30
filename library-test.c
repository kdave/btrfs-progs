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

#include "kerncompat.h"
#include "version.h"
#include "send-stream.h"

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

int main() {
	test_send_stream_api();

	return 0;
}
