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

#ifndef __OPEN_UTILS_H__
#define __OPEN_UTILS_H__

#include <stddef.h>
#include <dirent.h>

struct btrfs_fs_devices;

int check_mounted_where(int fd, const char *file, char *where, int size,
			struct btrfs_fs_devices **fs_dev_ret, unsigned sbflags);
int check_mounted(const char* file);
int get_btrfs_mount(const char *dev, char *mp, size_t mp_size);
int open_path_or_dev_mnt(const char *path, DIR **dirstream, int verbose);

int open_file_or_dir3(const char *fname, DIR **dirstream, int open_flags);
int open_file_or_dir(const char *fname, DIR **dirstream);

int btrfs_open(const char *path, DIR **dirstream, int verbose, int dir_only);
int btrfs_open_dir(const char *path, DIR **dirstream, int verbose);
int btrfs_open_file_or_dir(const char *path, DIR **dirstream, int verbose);

void close_file_or_dir(int fd, DIR *dirstream);

#endif
