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

#ifndef __BTRFS_FILESYSTEM_UTILS_H__
#define __BTRFS_FILESYSTEM_UTILS_H__

#include "kerncompat.h"

int lookup_path_rootid(int fd, u64 *rootid);
int get_label(const char *btrfs_dev, char *label);
int set_label(const char *btrfs_dev, const char *label);
int get_label_mounted(const char *mount_path, char *labelp);
int get_label_unmounted(const char *dev, char *label);

#endif
