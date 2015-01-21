/*
 * Copyright (C) 2007 Oracle.  All rights reserved.
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

#ifndef __CMDS_FI_DISK_USAGE_H__
#define __CMDS_FI_DISK_USAGE_H__

extern const char * const cmd_filesystem_usage_usage[];
int cmd_filesystem_usage(int argc, char **argv);

struct device_info {
	u64	devid;
	char	path[BTRFS_DEVICE_PATH_NAME_MAX];
	/* Size of the block device */
	u64	device_size;
	/* Size that's occupied by the filesystem, can be changed via resize */
	u64     size;
};

/*
 * To store the size information about the chunks:
 * the chunks info are grouped by the tuple (type, devid, num_stripes),
 * i.e. if two chunks are of the same type (RAID1, DUP...), are on the
 * same disk, have the same stripes then their sizes are grouped
 */
struct chunk_info {
	u64	type;
	u64	size;
	u64	devid;
	u64	num_stripes;
};

int load_chunk_and_device_info(int fd, struct chunk_info **chunkinfo,
		int *chunkcount, struct device_info **devinfo, int *devcount);
void print_device_chunks(int fd, struct device_info *devinfo,
		struct chunk_info *chunks_info_ptr,
		int chunks_info_count, unsigned unit_mode);
void print_device_sizes(int fd, struct device_info *devinfo, unsigned unit_mode);

#endif
