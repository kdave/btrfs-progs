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

#ifndef __DEVICE_SCAN_H__
#define __DEVICE_SCAN_H__

#include "kerncompat.h"
#include <dirent.h>
#include "ioctl.h"

#define BTRFS_SCAN_MOUNTED	(1ULL << 0)
#define BTRFS_SCAN_LBLKID	(1ULL << 1)

#define BTRFS_UPDATE_KERNEL	1

#define BTRFS_ARG_UNKNOWN	0
#define BTRFS_ARG_MNTPOINT	1
#define BTRFS_ARG_UUID		2
#define BTRFS_ARG_BLKDEV	3
#define BTRFS_ARG_REG		4

#define SEEN_FSID_HASH_SIZE 256

struct btrfs_root;
struct btrfs_trans_handle;
struct seen_fsid;
struct DIR;

struct seen_fsid {
	u8 fsid[BTRFS_FSID_SIZE];
	struct seen_fsid *next;
	DIR *dirstream;
	int fd;
};

int btrfs_scan_devices(int verbose);
int btrfs_register_one_device(const char *fname);
int btrfs_register_all_devices(void);
int btrfs_add_to_fsid(struct btrfs_trans_handle *trans,
		      struct btrfs_root *root, int fd, const char *path,
		      u64 device_total_bytes, u32 io_width, u32 io_align,
		      u32 sectorsize);
int btrfs_device_already_in_root(struct btrfs_root *root, int fd,
				 int super_offset);
int is_seen_fsid(u8 *fsid, struct seen_fsid *seen_fsid_hash[]);
int add_seen_fsid(u8 *fsid, struct seen_fsid *seen_fsid_hash[],
		int fd, DIR *dirstream);
void free_seen_fsid(struct seen_fsid *seen_fsid_hash[]);
int test_uuid_unique(const char *uuid_str);

#endif
