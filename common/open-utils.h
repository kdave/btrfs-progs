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
#include <stdbool.h>

struct btrfs_fs_devices;

int check_mounted_where(int fd, const char *file, char *where, int size,
			struct btrfs_fs_devices **fs_dev_ret, unsigned sbflags,
			bool noscan);
int check_mounted(const char* file);
int get_btrfs_mount(const char *dev, char *mp, size_t mp_size);

int btrfs_open_fd2(const char *path, bool verbose, bool read_write, bool dir_only);
int btrfs_open_file_or_dir_fd(const char *path);
int btrfs_open_dir_fd(const char *path);
int btrfs_open_mnt_fd(const char *path);

#endif
