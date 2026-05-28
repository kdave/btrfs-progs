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
#include <sys/stat.h>
#include <linux/limits.h>
#include <ftw.h>

char *path_canonicalize_dm_name(const char *ptname);
char *path_canonicalize(const char *path);

int arg_copy_path(char *dest, const char *src, int destlen);
int path_cat_out(char *out, const char *p1, const char *p2);
int path_cat3_out(char *out, const char *p1, const char *p2, const char *p3);
int path_is_block_device(const char *file);
int path_is_a_mount_point(const char *file);
int path_exists(const char *file);
int path_is_reg_file(const char *path);
int path_is_dir(const char *path);
int is_same_loop_file(const char *a, const char *b);
int path_is_reg_or_block_device(const char *filename);
int path_is_in_dir(const char *parent, const char *path);
char *path_basename(char *path);
char *path_dirname(char *path);
int path_readlink(char *dest, const char *src);

/*
 * Callback for path_sorted_walk(), matching nftw()'s callback signature
 * so existing nftw() callbacks can be reused unchanged.
 *
 *   st     : lstat() result, or NULL when type == FTW_NS
 *   type   : FTW_F, FTW_D (pre-order), FTW_SL (never followed),
 *            FTW_DNR (unreadable directory) or FTW_NS (lstat failed)
 *   ftwbuf : level (depth, 0 at the root) and base (basename offset)
 *
 * Return 0 to continue the walk, non-zero to abort it (that value is
 * then returned by path_sorted_walk()).
 */
typedef int (*path_walk_cb)(const char *path, const struct stat *st,
			    int type, struct FTW *ftwbuf);

/*
 * Walk a directory tree depth-first, pre-order, visiting the entries of
 * each directory in byte-wise (strcmp) name order. Symlinks are never
 * followed (FTW_PHYS). Sorting makes the walk order independent of the
 * filesystem's readdir() order.
 *
 * Returns 0 on success, the callback's non-zero return value if it
 * aborted, or -errno on I/O failure.
 */
int path_sorted_walk(const char *root, path_walk_cb cb);

#endif
