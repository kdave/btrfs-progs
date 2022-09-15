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

#ifndef __BTRFS_IMAGE_SANITIZE_H__
#define __BTRFS_IMAGE_SANITIZE_H__

#include "kerncompat.h"
#include "kernel-lib/rbtree_types.h"

struct btrfs_key;
struct extent_buffer;

struct name {
	struct rb_node n;
	char *val;
	char *sub;
	u32 len;
};

/*
 * Filenames and xattrs can be obfuscated so they don't appear in the image
 * dump. In basic mode (NAMES) a random string will be generated but such names
 * do not match the direntry hashes. The advanced mode (COLLISIONS) tries to
 * generate names that appear random but also match the hashes. This however
 * may take significantly more time than the basic mode. And may not even
 * succeed.
 */
enum sanitize_mode {
	SANITIZE_NONE,
	SANITIZE_NAMES,
	SANITIZE_COLLISIONS
};

void sanitize_name(enum sanitize_mode sanitize, struct rb_root *name_tree,
		u8 *dst, struct extent_buffer *src, struct btrfs_key *key,
		int slot);

#endif
