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

#include "kerncompat.h"
#include "kernel-shared/disk-io.h"
#include "kernel-shared/ctree.h"
#include "kernel-shared/free-space-cache.h"
#include "kernel-shared/free-space-tree.h"
#include "kernel-shared/volumes.h"
#include "kernel-shared/transaction.h"
#include "common/internal.h"
#include "common/messages.h"
#include "check/common.h"
#include "check/repair.h"
#include "check/mode-common.h"
#include "check/clear-cache.h"

/*
 * Number of free space cache inodes to delete in one transaction.
 *
 * This is to speedup the v1 space cache deletion for large fs.
 */
#define NR_BLOCK_GROUP_CLUSTER		(16)

static int clear_free_space_cache(void)
{
	struct btrfs_trans_handle *trans;
	struct btrfs_block_group *bg_cache;
	int nr_handled = 0;
	u64 current = 0;
	int ret = 0;

	trans = btrfs_start_transaction(gfs_info->tree_root, 0);
	if (IS_ERR(trans)) {
		ret = PTR_ERR(trans);
		errno = -ret;
		error_msg(ERROR_MSG_START_TRANS, "%m");
		return ret;
	}

	/* Clear all free space cache inodes and its extent data */
	while (1) {
		bg_cache = btrfs_lookup_first_block_group(gfs_info, current);
		if (!bg_cache)
			break;
		ret = btrfs_clear_free_space_cache(trans, bg_cache);
		if (ret < 0) {
			btrfs_abort_transaction(trans, ret);
			return ret;
		}
		nr_handled++;

		if (nr_handled == NR_BLOCK_GROUP_CLUSTER) {
			ret = btrfs_commit_transaction(trans, gfs_info->tree_root);
			if (ret < 0) {
				errno = -ret;
				error_msg(ERROR_MSG_START_TRANS, "%m");
				return ret;
			}
			trans = btrfs_start_transaction(gfs_info->tree_root, 0);
			if (IS_ERR(trans)) {
				ret = PTR_ERR(trans);
				errno = -ret;
				error_msg(ERROR_MSG_START_TRANS, "%m");
				return ret;
			}
		}
		current = bg_cache->start + bg_cache->length;
	}

	btrfs_set_super_cache_generation(gfs_info->super_copy, (u64)-1);
	ret = btrfs_commit_transaction(trans, gfs_info->tree_root);
	if (ret < 0) {
		errno = -ret;
		error_msg(ERROR_MSG_START_TRANS, "%m");
	}
	return ret;
}

int do_clear_free_space_cache(int clear_version)
{
	int ret = 0;

	if (clear_version == 1) {
		if (btrfs_fs_compat_ro(gfs_info, FREE_SPACE_TREE))
			warning(
"free space cache v2 detected, use --clear-space-cache v2, proceeding with clearing v1");

		ret = clear_free_space_cache();
		if (ret) {
			error("failed to clear free space cache");
			ret = 1;
		} else {
			printf("Free space cache cleared\n");
		}
	} else if (clear_version == 2) {
		if (!btrfs_fs_compat_ro(gfs_info, FREE_SPACE_TREE)) {
			printf("no free space cache v2 to clear\n");
			ret = 0;
			goto close_out;
		}
		printf("Clear free space cache v2\n");
		ret = btrfs_clear_free_space_tree(gfs_info);
		if (ret) {
			error("failed to clear free space cache v2: %d", ret);
			ret = 1;
		} else {
			printf("free space cache v2 cleared\n");
		}
	}
close_out:
	return ret;
}

static int check_free_space_tree(struct btrfs_root *root)
{
	struct btrfs_key key = { 0 };
	struct btrfs_path path;
	int ret = 0;

	btrfs_init_path(&path);

	while (1) {
		struct btrfs_block_group *bg;
		u64 cur_start = key.objectid;

		ret = btrfs_search_slot(NULL, root, &key, &path, 0, 0);
		if (ret < 0)
			goto out;

		/*
		 * We should be landing on an item, so if we're above the
		 * nritems we know we hit the end of the tree.
		 */
		if (path.slots[0] >= btrfs_header_nritems(path.nodes[0]))
			break;

		btrfs_item_key_to_cpu(path.nodes[0], &key, path.slots[0]);

		if (key.type != BTRFS_FREE_SPACE_INFO_KEY) {
			fprintf(stderr,
			"Failed to find a space info key at %llu [%llu %u %llu]\n",
				cur_start, key.objectid, key.type, key.offset);
			ret = -EINVAL;
			goto out;
		}

		bg = btrfs_lookup_first_block_group(gfs_info, key.objectid);
		if (!bg) {
			fprintf(stderr,
		"We have a space info key for a block group that doesn't exist\n");
			ret = -EINVAL;
			goto out;
		}

		btrfs_release_path(&path);
		key.objectid += key.offset;
		key.offset = 0;
	}
	ret = 0;
out:
	btrfs_release_path(&path);
	return ret;
}

static int check_free_space_trees(struct btrfs_root *root)
{
	struct btrfs_root *free_space_root;
	struct rb_node *n;
	struct btrfs_key key = {
		.objectid = BTRFS_FREE_SPACE_TREE_OBJECTID,
		.type = BTRFS_ROOT_ITEM_KEY,
		.offset = 0,
	};
	int ret = 0;

	free_space_root = btrfs_global_root(gfs_info, &key);
	while (1) {
		ret = check_free_space_tree(free_space_root);
		if (ret)
			break;
		n = rb_next(&root->rb_node);
		if (!n)
			break;
		free_space_root = rb_entry(n, struct btrfs_root, rb_node);
		if (root->root_key.objectid != BTRFS_FREE_SPACE_TREE_OBJECTID)
			break;
	}
	return ret;
}

static int check_cache_range(struct btrfs_root *root,
			     struct btrfs_block_group *cache,
			     u64 offset, u64 bytes)
{
	struct btrfs_free_space *entry;
	u64 *logical;
	u64 bytenr;
	int stripe_len;
	int i, nr, ret;

	for (i = 0; i < BTRFS_SUPER_MIRROR_MAX; i++) {
		bytenr = btrfs_sb_offset(i);
		ret = btrfs_rmap_block(gfs_info,
				       cache->start, bytenr,
				       &logical, &nr, &stripe_len);
		if (ret)
			return ret;

		while (nr--) {
			if (logical[nr] + stripe_len <= offset)
				continue;
			if (offset + bytes <= logical[nr])
				continue;
			if (logical[nr] == offset) {
				if (stripe_len >= bytes) {
					free(logical);
					return 0;
				}
				bytes -= stripe_len;
				offset += stripe_len;
			} else if (logical[nr] < offset) {
				if (logical[nr] + stripe_len >=
				    offset + bytes) {
					free(logical);
					return 0;
				}
				bytes = (offset + bytes) -
					(logical[nr] + stripe_len);
				offset = logical[nr] + stripe_len;
			} else {
				/*
				 * Could be tricky, the super may land in the
				 * middle of the area we're checking.  First
				 * check the easiest case, it's at the end.
				 */
				if (logical[nr] + stripe_len >=
				    bytes + offset) {
					bytes = logical[nr] - offset;
					continue;
				}

				/* Check the left side */
				ret = check_cache_range(root, cache,
							offset,
							logical[nr] - offset);
				if (ret) {
					free(logical);
					return ret;
				}

				/* Now we continue with the right side */
				bytes = (offset + bytes) -
					(logical[nr] + stripe_len);
				offset = logical[nr] + stripe_len;
			}
		}

		free(logical);
	}

	entry = btrfs_find_free_space(cache->free_space_ctl, offset, bytes);
	if (!entry) {
		fprintf(stderr, "there is no free space entry for %llu-%llu\n",
			offset, offset+bytes);
		return -EINVAL;
	}

	if (entry->offset != offset) {
		fprintf(stderr, "wanted offset %llu, found %llu\n", offset,
			entry->offset);
		return -EINVAL;
	}

	if (entry->bytes != bytes) {
		fprintf(stderr, "wanted bytes %llu, found %llu for off %llu\n",
			bytes, entry->bytes, offset);
		return -EINVAL;
	}

	unlink_free_space(cache->free_space_ctl, entry);
	free(entry);
	return 0;
}

static int verify_space_cache(struct btrfs_root *root,
			      struct btrfs_block_group *cache,
			      struct extent_io_tree *used)
{
	u64 start, end, last_end, bg_end;
	int ret = 0;

	start = cache->start;
	bg_end = cache->start + cache->length;
	last_end = start;

	while (start < bg_end) {
		ret = find_first_extent_bit(used, cache->start, &start, &end,
					    EXTENT_DIRTY);
		if (ret || start >= bg_end) {
			ret = 0;
			break;
		}
		if (last_end < start) {
			ret = check_cache_range(root, cache, last_end,
						start - last_end);
			if (ret)
				return ret;
		}
		end = min(end, bg_end - 1);
		clear_extent_dirty(used, start, end);
		start = end + 1;
		last_end = start;
	}

	if (last_end < bg_end)
		ret = check_cache_range(root, cache, last_end,
					bg_end - last_end);

	if (!ret &&
	    !RB_EMPTY_ROOT(&cache->free_space_ctl->free_space_offset)) {
		fprintf(stderr, "There are still entries left in the space "
			"cache\n");
		ret = -EINVAL;
	}

	return ret;
}

static int check_space_cache(struct btrfs_root *root)
{
	struct extent_io_tree used;
	struct btrfs_block_group *cache;
	u64 start = BTRFS_SUPER_INFO_OFFSET + BTRFS_SUPER_INFO_SIZE;
	int ret;
	int error = 0;

	extent_io_tree_init(&used);
	ret = btrfs_mark_used_blocks(gfs_info, &used);
	if (ret)
		return ret;

	while (1) {
		g_task_ctx.item_count++;
		cache = btrfs_lookup_first_block_group(gfs_info, start);
		if (!cache)
			break;

		start = cache->start + cache->length;
		if (!cache->free_space_ctl) {
			if (btrfs_init_free_space_ctl(cache,
						gfs_info->sectorsize)) {
				ret = -ENOMEM;
				break;
			}
		} else {
			btrfs_remove_free_space_cache(cache);
		}

		if (btrfs_fs_compat_ro(gfs_info, FREE_SPACE_TREE)) {
			ret = exclude_super_stripes(gfs_info, cache);
			if (ret) {
				errno = -ret;
				fprintf(stderr,
					"could not exclude super stripes: %m\n");
				error++;
				continue;
			}
			ret = load_free_space_tree(gfs_info, cache);
			free_excluded_extents(gfs_info, cache);
			if (ret < 0) {
				errno = -ret;
				fprintf(stderr,
					"could not load free space tree: %m\n");
				error++;
				continue;
			}
			error += ret;
		} else {
			ret = load_free_space_cache(gfs_info, cache);
			if (ret < 0)
				error++;
			if (ret <= 0)
				continue;
		}

		ret = verify_space_cache(root, cache, &used);
		if (ret) {
			fprintf(stderr, "cache appears valid but isn't %llu\n",
				cache->start);
			error++;
		}
	}
	extent_io_tree_cleanup(&used);
	return error ? -EINVAL : 0;
}


int validate_free_space_cache(struct btrfs_root *root)
{
	int ret;

	/*
	 * If cache generation is between 0 and -1ULL, sb generation must be
	 * equal to sb cache generation or the v1 space caches are outdated.
	 */
	if (btrfs_super_cache_generation(gfs_info->super_copy) != -1ULL &&
	    btrfs_super_cache_generation(gfs_info->super_copy) != 0 &&
	    btrfs_super_generation(gfs_info->super_copy) !=
	    btrfs_super_cache_generation(gfs_info->super_copy)) {
		printf(
"cache and super generation don't match, space cache will be invalidated\n");
		return 0;
	}

	ret = check_space_cache(root);
	if (!ret && btrfs_fs_compat_ro(gfs_info, FREE_SPACE_TREE))
		ret = check_free_space_trees(root);
	if (ret && btrfs_fs_compat_ro(gfs_info, FREE_SPACE_TREE) &&
	    opt_check_repair) {
		ret = do_clear_free_space_cache(2);
		if (ret)
			goto out;

		ret = btrfs_create_free_space_tree(gfs_info);
		if (ret)
			error("couldn't repair freespace tree");
	}

out:
	return ret ? -EINVAL : 0;
}

int truncate_free_ino_items(struct btrfs_root *root)
{
	struct btrfs_path path;
	struct btrfs_key key = { .objectid = BTRFS_FREE_INO_OBJECTID,
				 .type = (u8)-1,
				 .offset = (u64)-1 };
	struct btrfs_trans_handle *trans;
	int ret;

	trans = btrfs_start_transaction(root, 0);
	if (IS_ERR(trans)) {
		error_msg(ERROR_MSG_START_TRANS, "inode-cache removal");
		return PTR_ERR(trans);
	}

	while (1) {
		struct extent_buffer *leaf;
		struct btrfs_file_extent_item *fi;
		struct btrfs_key found_key;
		u8 found_type;

		btrfs_init_path(&path);
		ret = btrfs_search_slot(trans, root, &key, &path, -1, 1);
		if (ret < 0) {
			btrfs_abort_transaction(trans, ret);
			goto out;
		} else if (ret > 0) {
			ret = 0;
			/* No more items, finished truncating */
			if (path.slots[0] == 0) {
				btrfs_release_path(&path);
				goto out;
			}
			path.slots[0]--;
		}
		fi = NULL;
		leaf = path.nodes[0];
		btrfs_item_key_to_cpu(leaf, &found_key, path.slots[0]);
		found_type = found_key.type;

		/* Ino cache also has free space bitmaps in the fs stree */
		if (found_key.objectid != BTRFS_FREE_INO_OBJECTID &&
		    found_key.objectid != BTRFS_FREE_SPACE_OBJECTID) {
			btrfs_release_path(&path);
			/* Now delete the FREE_SPACE_OBJECTID */
			if (key.objectid == BTRFS_FREE_INO_OBJECTID) {
				key.objectid = BTRFS_FREE_SPACE_OBJECTID;
				continue;
			}
			break;
		}

		if (found_type == BTRFS_EXTENT_DATA_KEY) {
			int extent_type;
			u64 extent_disk_bytenr;
			u64 extent_num_bytes;
			u64 extent_offset;

			fi = btrfs_item_ptr(leaf, path.slots[0],
					    struct btrfs_file_extent_item);
			extent_type = btrfs_file_extent_type(leaf, fi);
			ASSERT(extent_type == BTRFS_FILE_EXTENT_REG);
			extent_disk_bytenr = btrfs_file_extent_disk_bytenr(leaf, fi);
			extent_num_bytes = btrfs_file_extent_disk_num_bytes (leaf, fi);
			extent_offset = found_key.offset -
					btrfs_file_extent_offset(leaf, fi);
			ASSERT(extent_offset == 0);
			ret = btrfs_free_extent(trans, root, extent_disk_bytenr,
						extent_num_bytes, 0, root->objectid,
						BTRFS_FREE_INO_OBJECTID, 0);
			if (ret < 0) {
				btrfs_abort_transaction(trans, ret);
				btrfs_release_path(&path);
				goto out;
			}

			ret = btrfs_del_csums(trans, extent_disk_bytenr,
					      extent_num_bytes);
			if (ret < 0) {
				btrfs_abort_transaction(trans, ret);
				btrfs_release_path(&path);
				goto out;
			}
		}

		ret = btrfs_del_item(trans, root, &path);
		BUG_ON(ret);
		btrfs_release_path(&path);
	}

	btrfs_commit_transaction(trans, root);
out:
	return ret;
}

int clear_ino_cache_items(void)
{
	int ret;
	struct btrfs_path path;
	struct btrfs_key key;

	key.objectid = BTRFS_FS_TREE_OBJECTID;
	key.type = BTRFS_ROOT_ITEM_KEY;
	key.offset = 0;

	btrfs_init_path(&path);
	ret = btrfs_search_slot(NULL, gfs_info->tree_root, &key, &path,	0, 0);
	if (ret < 0)
		return ret;

	while(1) {
		struct btrfs_key found_key;

		btrfs_item_key_to_cpu(path.nodes[0], &found_key, path.slots[0]);
		if (found_key.type == BTRFS_ROOT_ITEM_KEY &&
		    is_fstree(found_key.objectid)) {
			struct btrfs_root *root;

			found_key.offset = (u64)-1;
			root = btrfs_read_fs_root(gfs_info, &found_key);
			if (IS_ERR(root))
				goto next;
			ret = truncate_free_ino_items(root);
			if (ret)
				goto out;
			printf("Successfully cleaned up ino cache for root id: %lld\n",
					root->objectid);
		} else {
			/* If we get a negative tree this means it's the last one */
			if ((s64)found_key.objectid < 0 &&
			    found_key.type == BTRFS_ROOT_ITEM_KEY)
				goto out;
		}

		/*
		 * Only fs roots contain an ino cache information - either
		 * FS_TREE_OBJECTID or subvol id >= BTRFS_FIRST_FREE_OBJECTID
		 */
next:
		if (key.objectid == BTRFS_FS_TREE_OBJECTID) {
			key.objectid = BTRFS_FIRST_FREE_OBJECTID;
			btrfs_release_path(&path);
			ret = btrfs_search_slot(NULL, gfs_info->tree_root, &key,
						&path,	0, 0);
			if (ret < 0)
				return ret;
		} else {
			ret = btrfs_next_item(gfs_info->tree_root, &path);
			if (ret < 0) {
				goto out;
			} else if (ret > 0) {
				ret = 0;
				goto out;
			}
		}
	}

out:
	btrfs_release_path(&path);
	return ret;
}

