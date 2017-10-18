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
 * License along with this program.
 */

/*
 * Defines and functions declarations for users mkfs --rootdir
 */

#ifndef __BTRFS_MKDIR_ROOTDIR_H__
#define __BTRFS_MKDIR_ROOTDIR_H__
struct directory_name_entry {
	const char *dir_name;
	const char *path;
	ino_t inum;
	struct list_head list;
};

int btrfs_mkfs_fill_dir(const char *source_dir, struct btrfs_root *root,
			bool verbose);
#endif
