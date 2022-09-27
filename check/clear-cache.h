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

#ifndef __BTRFS_CHECK_CLEAR_CACHE_H__
#define __BTRFS_CHECK_CLEAR_CACHE_H__

struct btrfs_root;

int do_clear_free_space_cache(int clear_version);
int validate_free_space_cache(struct btrfs_root *root);
int truncate_free_ino_items(struct btrfs_root *root);
int clear_ino_cache_items(void);

#endif
