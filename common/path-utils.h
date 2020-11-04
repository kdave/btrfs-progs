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

#ifndef __BTRFS_PATH_UTILS_H__
#define __BTRFS_PATH_UTILS_H__

#include <sys/types.h>

char *canonicalize_dm_name(const char *ptname);
char *canonicalize_path(const char *path);

int arg_copy_path(char *dest, const char *src, int destlen);
int path_cat_out(char *out, const char *p1, const char *p2);
int path_cat3_out(char *out, const char *p1, const char *p2, const char *p3);

char *__strncpy_null(char *dest, const char *src, size_t n);
/* Helper to always get proper size of the destination string */
#define strncpy_null(dest, src) __strncpy_null(dest, src, sizeof(dest))

int path_is_block_device(const char *file);
int path_is_mount_point(const char *file);
int path_exists(const char *file);
int path_is_reg_file(const char *path);
int path_is_dir(const char *path);
int is_same_loop_file(const char *a, const char *b);
int path_is_reg_or_block_device(const char *filename);

int test_issubvolname(const char *name);

#endif
