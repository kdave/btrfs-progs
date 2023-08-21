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

#ifndef __COMMON_SYSFS_UTILS__
#define __COMMON_SYSFS_UTILS__

int sysfs_open_file(const char *name);
int sysfs_open_fsid_file(int fd, const char *filename);
int sysfs_open_fsid_dir(int fd, const char *dirname);
int sysfs_read_fsid_file_u64(int fd, const char *name, u64 *value);
int sysfs_read_file(int fd, char *buf, size_t size);
int sysfs_read_file_u64(const char *name, u64 *value);

#endif
