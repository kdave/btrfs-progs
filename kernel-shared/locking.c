/* SPDX-License-Identifier: GPL-2.0 */

#include "kernel-shared/ctree.h"
#include "kernel-shared/locking.h"

struct extent_buffer *btrfs_read_lock_root_node(struct btrfs_root *root)
{
	return root->node;
}

struct extent_buffer *btrfs_try_read_lock_root_node(struct btrfs_root *root)
{
	return root->node;
}

struct extent_buffer *btrfs_lock_root_node(struct btrfs_root *root)
{
	return root->node;
}
