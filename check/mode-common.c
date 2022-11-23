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
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include "kernel-lib/rbtree.h"
#include "kernel-shared/extent_io.h"
#include "kernel-shared/ctree.h"
#include "kernel-shared/transaction.h"
#include "kernel-shared/disk-io.h"
#include "kernel-shared/volumes.h"
#include "kernel-shared/backref.h"
#include "kernel-shared/compression.h"
#include "common/internal.h"
#include "common/messages.h"
#include "common/utils.h"
#include "check/mode-common.h"
#include "check/repair.h"

struct task_ctx g_task_ctx = { 0 };

/*
 * Check if the inode referenced by the given data reference uses the extent
 * at disk_bytenr as a non-prealloc extent.
 *
 * Returns 1 if true, 0 if false and < 0 on error.
 */
static int check_prealloc_data_ref(u64 disk_bytenr,
				   struct btrfs_extent_data_ref *dref,
				   struct extent_buffer *eb)
{
	u64 rootid = btrfs_extent_data_ref_root(eb, dref);
	u64 objectid = btrfs_extent_data_ref_objectid(eb, dref);
	u64 offset = btrfs_extent_data_ref_offset(eb, dref);
	struct btrfs_root *root;
	struct btrfs_key key;
	struct btrfs_path path;
	int ret;

	btrfs_init_path(&path);
	key.objectid = rootid;
	key.type = BTRFS_ROOT_ITEM_KEY;
	key.offset = (u64)-1;
	root = btrfs_read_fs_root(gfs_info, &key);
	if (IS_ERR(root)) {
		ret = PTR_ERR(root);
		goto out;
	}

	key.objectid = objectid;
	key.type = BTRFS_EXTENT_DATA_KEY;
	key.offset = offset;
	ret = btrfs_search_slot(NULL, root, &key, &path, 0, 0);
	if (ret > 0) {
		fprintf(stderr,
		"Missing file extent item for inode %llu, root %llu, offset %llu",
			objectid, rootid, offset);
		ret = -ENOENT;
	}
	if (ret < 0)
		goto out;

	while (true) {
		struct btrfs_file_extent_item *fi;
		int extent_type;

		if (path.slots[0] >= btrfs_header_nritems(path.nodes[0])) {
			ret = btrfs_next_leaf(root, &path);
			if (ret < 0)
				goto out;
			if (ret > 0)
				break;
		}

		btrfs_item_key_to_cpu(path.nodes[0], &key, path.slots[0]);
		if (key.objectid != objectid ||
		    key.type != BTRFS_EXTENT_DATA_KEY)
			break;

		fi = btrfs_item_ptr(path.nodes[0], path.slots[0],
				    struct btrfs_file_extent_item);
		extent_type = btrfs_file_extent_type(path.nodes[0], fi);
		if (extent_type != BTRFS_FILE_EXTENT_REG &&
		    extent_type != BTRFS_FILE_EXTENT_PREALLOC)
			goto next;

		if (btrfs_file_extent_disk_bytenr(path.nodes[0], fi) !=
		    disk_bytenr)
			break;

		if (extent_type == BTRFS_FILE_EXTENT_REG) {
			ret = 1;
			goto out;
		}
next:
		path.slots[0]++;
	}
	ret = 0;
 out:
	btrfs_release_path(&path);
	return ret;
}

/*
 * Check if a shared data reference points to a node that has a file extent item
 * pointing to the extent at @disk_bytenr that is not of type prealloc.
 *
 * Returns 1 if true, 0 if false and < 0 on error.
 */
static int check_prealloc_shared_data_ref(u64 parent, u64 disk_bytenr)
{
	struct extent_buffer *eb;
	u32 nr;
	int i;
	int ret = 0;

	eb = read_tree_block(gfs_info, parent, 0);
	if (!extent_buffer_uptodate(eb)) {
		ret = -EIO;
		goto out;
	}

	nr = btrfs_header_nritems(eb);
	for (i = 0; i < nr; i++) {
		struct btrfs_key key;
		struct btrfs_file_extent_item *fi;
		int extent_type;

		btrfs_item_key_to_cpu(eb, &key, i);
		if (key.type != BTRFS_EXTENT_DATA_KEY)
			continue;

		fi = btrfs_item_ptr(eb, i, struct btrfs_file_extent_item);
		extent_type = btrfs_file_extent_type(eb, fi);
		if (extent_type != BTRFS_FILE_EXTENT_REG &&
		    extent_type != BTRFS_FILE_EXTENT_PREALLOC)
			continue;

		if (btrfs_file_extent_disk_bytenr(eb, fi) == disk_bytenr &&
		    extent_type == BTRFS_FILE_EXTENT_REG) {
			ret = 1;
			break;
		}
	}
 out:
	free_extent_buffer(eb);
	return ret;
}

/*
 * Check if a prealloc extent is shared by multiple inodes and if any inode has
 * already written to that extent. This is to avoid emitting invalid warnings
 * about odd csum items (a inode has an extent entirely marked as prealloc
 * but another inode shares it and has already written to it).
 *
 * Note: right now it does not check if the number of checksum items in the
 * csum tree matches the number of bytes written into the ex-prealloc extent.
 * It's complex to deal with that because the prealloc extent might have been
 * partially written through multiple inodes and we would have to keep track of
 * ranges, merging them and notice ranges that fully or partially overlap, to
 * avoid false reports of csum items missing for areas of the prealloc extent
 * that were not written to - for example if we have a 1M prealloc extent, we
 * can have only the first half of it written, but 2 different inodes refer to
 * the its first half (through reflinks/cloning), so keeping a counter of bytes
 * covered by checksum items is not enough, as the correct value would be 512K
 * and not 1M (whence the need to track ranges).
 *
 * Returns 0 if the prealloc extent was not written yet by any inode, 1 if
 * at least one other inode has written to it, and < 0 on error.
 */
int check_prealloc_extent_written(u64 disk_bytenr, u64 num_bytes)
{
	struct btrfs_root *extent_root = btrfs_extent_root(gfs_info,
							   disk_bytenr);
	struct btrfs_path path;
	struct btrfs_key key;
	int ret;
	struct btrfs_extent_item *ei;
	u32 item_size;
	unsigned long ptr;
	unsigned long end;

	key.objectid = disk_bytenr;
	key.type = BTRFS_EXTENT_ITEM_KEY;
	key.offset = num_bytes;

	btrfs_init_path(&path);
	ret = btrfs_search_slot(NULL, extent_root, &key, &path, 0, 0);
	if (ret > 0) {
		fprintf(stderr,
	"Missing extent item in extent tree for disk_bytenr %llu, num_bytes %llu\n",
			disk_bytenr, num_bytes);
		ret = -ENOENT;
	}
	if (ret < 0)
		goto out;

	/* First check all inline refs. */
	ei = btrfs_item_ptr(path.nodes[0], path.slots[0],
			    struct btrfs_extent_item);
	item_size = btrfs_item_size(path.nodes[0], path.slots[0]);
	ptr = (unsigned long)(ei + 1);
	end = (unsigned long)ei + item_size;
	while (ptr < end) {
		struct btrfs_extent_inline_ref *iref;
		int type;

		iref = (struct btrfs_extent_inline_ref *)ptr;
		type = btrfs_extent_inline_ref_type(path.nodes[0], iref);
		ASSERT(type == BTRFS_EXTENT_DATA_REF_KEY ||
		       type == BTRFS_SHARED_DATA_REF_KEY);

		if (type == BTRFS_EXTENT_DATA_REF_KEY) {
			struct btrfs_extent_data_ref *dref;

			dref = (struct btrfs_extent_data_ref *)(&iref->offset);
			ret = check_prealloc_data_ref(disk_bytenr,
						      dref, path.nodes[0]);
			if (ret != 0)
				goto out;
		} else if (type == BTRFS_SHARED_DATA_REF_KEY) {
			u64 parent;

			parent = btrfs_extent_inline_ref_offset(path.nodes[0],
								iref);
			ret = check_prealloc_shared_data_ref(parent,
							     disk_bytenr);
			if (ret != 0)
				goto out;
		}

		ptr += btrfs_extent_inline_ref_size(type);
	}

	/* Now check if there are any non-inlined refs. */
	path.slots[0]++;
	while (true) {
		if (path.slots[0] >= btrfs_header_nritems(path.nodes[0])) {
			ret = btrfs_next_leaf(extent_root, &path);
			if (ret < 0)
				goto out;
			if (ret > 0) {
				ret = 0;
				break;
			}
		}

		btrfs_item_key_to_cpu(path.nodes[0], &key, path.slots[0]);
		if (key.objectid != disk_bytenr)
			break;

		if (key.type == BTRFS_EXTENT_DATA_REF_KEY) {
			struct btrfs_extent_data_ref *dref;

			dref = btrfs_item_ptr(path.nodes[0], path.slots[0],
					      struct btrfs_extent_data_ref);
			ret = check_prealloc_data_ref(disk_bytenr,
						      dref, path.nodes[0]);
			if (ret != 0)
				goto out;
		} else if (key.type == BTRFS_SHARED_DATA_REF_KEY) {
			ret = check_prealloc_shared_data_ref(key.offset,
							     disk_bytenr);
			if (ret != 0)
				goto out;
		}

		path.slots[0]++;
	}
out:
	btrfs_release_path(&path);
	return ret;
}

/*
 * Search in csum tree to find how many bytes of range [@start, @start + @len)
 * has the corresponding csum item.
 *
 * @start:	range start
 * @len:	range length
 * @found:	return value of found csum bytes
 *		unit is BYTE.
 */
int count_csum_range(u64 start, u64 len, u64 *found)
{
	struct btrfs_root *csum_root = btrfs_csum_root(gfs_info, start);
	struct btrfs_key key;
	struct btrfs_path path;
	struct extent_buffer *leaf;
	int ret;
	size_t size;
	*found = 0;
	u64 csum_end;
	u16 csum_size = gfs_info->csum_size;

	btrfs_init_path(&path);

	key.objectid = BTRFS_EXTENT_CSUM_OBJECTID;
	key.offset = start;
	key.type = BTRFS_EXTENT_CSUM_KEY;

	ret = btrfs_search_slot(NULL, csum_root, &key, &path, 0, 0);
	if (ret < 0)
		goto out;
	if (ret > 0 && path.slots[0] > 0) {
		leaf = path.nodes[0];
		btrfs_item_key_to_cpu(leaf, &key, path.slots[0] - 1);
		if (key.objectid == BTRFS_EXTENT_CSUM_OBJECTID &&
		    key.type == BTRFS_EXTENT_CSUM_KEY)
			path.slots[0]--;
	}

	while (len > 0) {
		leaf = path.nodes[0];
		if (path.slots[0] >= btrfs_header_nritems(leaf)) {
			ret = btrfs_next_leaf(csum_root, &path);
			if (ret > 0)
				break;
			else if (ret < 0)
				goto out;
			leaf = path.nodes[0];
		}

		btrfs_item_key_to_cpu(leaf, &key, path.slots[0]);
		if (key.objectid != BTRFS_EXTENT_CSUM_OBJECTID ||
		    key.type != BTRFS_EXTENT_CSUM_KEY)
			break;

		btrfs_item_key_to_cpu(leaf, &key, path.slots[0]);
		if (key.offset >= start + len)
			break;

		if (key.offset > start)
			start = key.offset;

		size = btrfs_item_size(leaf, path.slots[0]);
		csum_end = key.offset + (size / csum_size) *
			   gfs_info->sectorsize;
		if (csum_end > start) {
			size = min(csum_end - start, len);
			len -= size;
			start += size;
			*found += size;
		}

		path.slots[0]++;
	}
out:
	btrfs_release_path(&path);
	if (ret < 0)
		return ret;
	return 0;
}

/*
 * Wrapper to insert one inode item into given @root
 * Timestamp will be set to current time.
 *
 * @root:	the root to insert inode item into
 * @ino:	inode number
 * @size:	inode size
 * @nbytes:	nbytes (real used size, without hole)
 * @nlink:	number of links
 * @mode:	file mode, including S_IF* bits
 */
int insert_inode_item(struct btrfs_trans_handle *trans,
		      struct btrfs_root *root, u64 ino, u64 size,
		      u64 nbytes, u64 nlink, u32 mode)
{
	struct btrfs_inode_item ii;
	time_t now = time(NULL);
	int ret;

	memset(&ii, 0, sizeof(ii));
	btrfs_set_stack_inode_size(&ii, size);
	btrfs_set_stack_inode_nbytes(&ii, nbytes);
	btrfs_set_stack_inode_nlink(&ii, nlink);
	btrfs_set_stack_inode_mode(&ii, mode);
	btrfs_set_stack_inode_generation(&ii, trans->transid);
	btrfs_set_stack_timespec_sec(&ii.ctime, now);
	btrfs_set_stack_timespec_sec(&ii.mtime, now);

	ret = btrfs_insert_inode(trans, root, ino, &ii);
	ASSERT(!ret);

	warning("root %llu inode %llu recreating inode item, this may "
		"be incomplete, please check permissions and content after "
		"the fsck completes.\n", (unsigned long long)root->objectid,
		(unsigned long long)ino);

	return 0;
}

static int get_highest_inode(struct btrfs_trans_handle *trans,
			     struct btrfs_root *root, struct btrfs_path *path,
			     u64 *highest_ino)
{
	struct btrfs_key key, found_key;
	int ret;

	btrfs_init_path(path);
	key.objectid = BTRFS_LAST_FREE_OBJECTID;
	key.offset = -1;
	key.type = BTRFS_INODE_ITEM_KEY;
	ret = btrfs_search_slot(trans, root, &key, path, -1, 1);
	if (ret == 1) {
		btrfs_item_key_to_cpu(path->nodes[0], &found_key,
				path->slots[0] - 1);
		*highest_ino = found_key.objectid;
		ret = 0;
	}
	if (*highest_ino >= BTRFS_LAST_FREE_OBJECTID)
		ret = -EOVERFLOW;
	btrfs_release_path(path);
	return ret;
}

/*
 * Link inode to dir 'lost+found'. Increase @ref_count.
 *
 * Returns 0 means success.
 * Returns <0 means failure.
 */
int link_inode_to_lostfound(struct btrfs_trans_handle *trans,
			    struct btrfs_root *root,
			    struct btrfs_path *path,
			    u64 ino, char *namebuf, u32 name_len,
			    u8 filetype, u64 *ref_count)
{
	char *dir_name = "lost+found";
	u64 lost_found_ino;
	int ret;
	u32 mode = 0700;

	btrfs_release_path(path);
	ret = get_highest_inode(trans, root, path, &lost_found_ino);
	if (ret < 0)
		goto out;
	lost_found_ino++;

	ret = btrfs_mkdir(trans, root, dir_name, strlen(dir_name),
			  BTRFS_FIRST_FREE_OBJECTID, &lost_found_ino,
			  mode);
	if (ret < 0) {
		errno = -ret;
		error("failed to create '%s' dir: %m", dir_name);
		goto out;
	}
	ret = btrfs_add_link(trans, root, ino, lost_found_ino,
			     namebuf, name_len, filetype, NULL, 1, 0);
	/*
	 * Add ".INO" suffix several times to handle case where
	 * "FILENAME.INO" is already taken by another file.
	 */
	while (ret == -EEXIST) {
		/*
		 * Conflicting file name, add ".INO" as suffix * +1 for '.'
		 */
		if (name_len + count_digits(ino) + 1 > BTRFS_NAME_LEN) {
			ret = -EFBIG;
			goto out;
		}
		snprintf(namebuf + name_len, BTRFS_NAME_LEN - name_len,
			 ".%llu", ino);
		name_len += count_digits(ino) + 1;
		ret = btrfs_add_link(trans, root, ino, lost_found_ino, namebuf,
				     name_len, filetype, NULL, 1, 0);
	}
	if (ret < 0) {
		errno = -ret;
		error("failed to link the inode %llu to %s dir: %m",
		      ino, dir_name);
		goto out;
	}

	++*ref_count;
	printf("Moving file '%.*s' to '%s' dir since it has no valid backref\n",
	       name_len, namebuf, dir_name);
out:
	btrfs_release_path(path);
	if (ret)
		error("failed to move file '%.*s' to '%s' dir", name_len,
				namebuf, dir_name);
	return ret;
}

/*
 * Extra (optional) check for dev_item size to report possible problem on a new
 * kernel.
 */
void check_dev_size_alignment(u64 devid, u64 total_bytes, u32 sectorsize)
{
	if (!IS_ALIGNED(total_bytes, sectorsize)) {
		warning(
"unaligned total_bytes detected for devid %llu, have %llu should be aligned to %u",
			devid, total_bytes, sectorsize);
		warning(
"this is OK for older kernel, but may cause kernel warning for newer kernels");
		warning("this can be fixed by 'btrfs rescue fix-device-size'");
	}
}

void reada_walk_down(struct btrfs_root *root, struct extent_buffer *node,
		     int slot)
{
	u64 bytenr;
	u64 ptr_gen;
	u32 nritems;
	int i;
	int level;

	level = btrfs_header_level(node);
	if (level != 1)
		return;

	nritems = btrfs_header_nritems(node);
	for (i = slot; i < nritems; i++) {
		bytenr = btrfs_node_blockptr(node, i);
		ptr_gen = btrfs_node_ptr_generation(node, i);
		readahead_tree_block(gfs_info, bytenr, ptr_gen);
	}
}

/*
 * Check the child node/leaf by the following condition:
 * 1. the first item key of the node/leaf should be the same with the one
 *    in parent.
 * 2. block in parent node should match the child node/leaf.
 * 3. generation of parent node and child's header should be consistent.
 *
 * Or the child node/leaf pointed by the key in parent is not valid.
 *
 * We hope to check leaf owner too, but since subvol may share leaves,
 * which makes leaf owner check not so strong, key check should be
 * sufficient enough for that case.
 */
int check_child_node(struct extent_buffer *parent, int slot,
		     struct extent_buffer *child)
{
	struct btrfs_key parent_key;
	struct btrfs_key child_key;
	int ret = 0;

	btrfs_node_key_to_cpu(parent, &parent_key, slot);
	if (btrfs_header_level(child) == 0)
		btrfs_item_key_to_cpu(child, &child_key, 0);
	else
		btrfs_node_key_to_cpu(child, &child_key, 0);

	if (memcmp(&parent_key, &child_key, sizeof(parent_key))) {
		ret = -EINVAL;
		fprintf(stderr,
			"Wrong key of child node/leaf, wanted: (%llu, %u, %llu), have: (%llu, %u, %llu)\n",
			parent_key.objectid, parent_key.type, parent_key.offset,
			child_key.objectid, child_key.type, child_key.offset);
	}
	if (btrfs_header_bytenr(child) != btrfs_node_blockptr(parent, slot)) {
		ret = -EINVAL;
		fprintf(stderr, "Wrong block of child node/leaf, wanted: %llu, have: %llu\n",
			btrfs_node_blockptr(parent, slot),
			btrfs_header_bytenr(child));
	}
	if (btrfs_node_ptr_generation(parent, slot) !=
	    btrfs_header_generation(child)) {
		ret = -EINVAL;
		fprintf(stderr, "Wrong generation of child node/leaf, wanted: %llu, have: %llu\n",
			btrfs_header_generation(child),
			btrfs_node_ptr_generation(parent, slot));
	}
	return ret;
}

void reset_cached_block_groups()
{
	struct btrfs_block_group *cache;
	u64 start, end;
	int ret;

	while (1) {
		ret = find_first_extent_bit(&gfs_info->free_space_cache, 0,
					    &start, &end, EXTENT_DIRTY);
		if (ret)
			break;
		clear_extent_dirty(&gfs_info->free_space_cache, start, end);
	}

	start = 0;
	while (1) {
		cache = btrfs_lookup_first_block_group(gfs_info, start);
		if (!cache)
			break;
		if (cache->cached)
			cache->cached = 0;
		start = cache->start + cache->length;
	}
}

int pin_metadata_blocks(void)
{
	return btrfs_mark_used_tree_blocks(gfs_info,
					   &gfs_info->pinned_extents);
}

int exclude_metadata_blocks(void)
{
	struct extent_io_tree *excluded_extents;

	excluded_extents = malloc(sizeof(*excluded_extents));
	if (!excluded_extents)
		return -ENOMEM;
	extent_io_tree_init(excluded_extents);
	gfs_info->excluded_extents = excluded_extents;

	return btrfs_mark_used_tree_blocks(gfs_info, excluded_extents);
}

void cleanup_excluded_extents(void)
{
	if (gfs_info->excluded_extents) {
		extent_io_tree_cleanup(gfs_info->excluded_extents);
		free(gfs_info->excluded_extents);
	}
	gfs_info->excluded_extents = NULL;
}

/*
 * Delete one corrupted dir item whose hash doesn't match its name.
 *
 * Since its hash is incorrect, we can't use btrfs_name_hash() to calculate
 * the search key, but rely on @di_key parameter to do the search.
 */
int delete_corrupted_dir_item(struct btrfs_trans_handle *trans,
			      struct btrfs_root *root,
			      struct btrfs_key *di_key, char *namebuf,
			      u32 namelen)
{
	struct btrfs_dir_item *di_item;
	struct btrfs_path path;
	int ret;

	btrfs_init_path(&path);
	ret = btrfs_search_slot(trans, root, di_key, &path, 0, 1);
	if (ret > 0) {
		error("key (%llu %u %llu) doesn't exist in root %llu",
			di_key->objectid, di_key->type, di_key->offset,
			root->root_key.objectid);
		ret = -ENOENT;
		goto out;
	}
	if (ret < 0) {
		error("failed to search root %llu: %d",
			root->root_key.objectid, ret);
		goto out;
	}

	di_item = btrfs_match_dir_item_name(root, &path, namebuf, namelen);
	if (!di_item) {
		/*
		 * This is possible if the dir_item has incorrect namelen.
		 * But in that case, we shouldn't reach repair path here.
		 */
		error("no dir item named '%s' found with key (%llu %u %llu)",
			namebuf, di_key->objectid, di_key->type,
			di_key->offset);
		ret = -ENOENT;
		goto out;
	}
	ret = btrfs_delete_one_dir_name(trans, root, &path, di_item);
	if (ret < 0)
		error("failed to delete one dir name: %d", ret);

out:
	btrfs_release_path(&path);
	return ret;
}

/*
 * Reset the mode of inode (specified by @root and @ino) to @mode.
 *
 * Caller should ensure @path is not populated, the @path is mainly for caller
 * to grab the correct new path of the inode.
 *
 * Return 0 if repair is done, @path will point to the correct inode item.
 * Return <0 for errors.
 */
int reset_imode(struct btrfs_trans_handle *trans, struct btrfs_root *root,
		struct btrfs_path *path, u64 ino, u32 mode)
{
	struct btrfs_inode_item *iitem;
	struct extent_buffer *leaf;
	struct btrfs_key key;
	int slot;
	int ret;

	key.objectid = ino;
	key.type = BTRFS_INODE_ITEM_KEY;
	key.offset = 0;

	ret = btrfs_search_slot(trans, root, &key, path, 0, 1);
	if (ret > 0)
		ret = -ENOENT;
	if (ret < 0) {
		errno = -ret;
		error("failed to search tree %llu: %m",
		      root->root_key.objectid);
		return ret;
	}
	leaf = path->nodes[0];
	slot = path->slots[0];
	iitem = btrfs_item_ptr(leaf, slot, struct btrfs_inode_item);
	btrfs_set_inode_mode(leaf, iitem, mode);
	btrfs_mark_buffer_dirty(leaf);
	return ret;
}

static int find_file_type_dir_index(struct btrfs_root *root, u64 ino, u64 dirid,
				    u64 index, const char *name, u32 name_len,
				    u32 *imode_ret)
{
	struct btrfs_path path;
	struct btrfs_key key;
	struct btrfs_key location;
	struct btrfs_dir_item *di;
	char namebuf[BTRFS_NAME_LEN] = {0};
	bool found = false;
	u8 filetype;
	u32 len;
	int ret;

	btrfs_init_path(&path);
	key.objectid = dirid;
	key.offset = index;
	key.type = BTRFS_DIR_INDEX_KEY;

	ret = btrfs_search_slot(NULL, root, &key, &path, 0, 0);
	if (ret > 0) {
		ret = -ENOENT;
		goto out;
	}
	if (ret < 0)
		goto out;
	di = btrfs_item_ptr(path.nodes[0], path.slots[0],
			    struct btrfs_dir_item);
	btrfs_dir_item_key_to_cpu(path.nodes[0], di, &location);

	/* Various basic check */
	if (location.objectid != ino || location.type != BTRFS_INODE_ITEM_KEY ||
	    location.offset != 0)
		goto out;
	filetype = btrfs_dir_type(path.nodes[0], di);
	if (filetype >= BTRFS_FT_MAX || filetype == BTRFS_FT_UNKNOWN)
		goto out;
	len = min_t(u32, BTRFS_NAME_LEN,
		btrfs_item_size(path.nodes[0], path.slots[0]) - sizeof(*di));
	len = min_t(u32, len, btrfs_dir_name_len(path.nodes[0], di));
	read_extent_buffer(path.nodes[0], namebuf, (unsigned long)(di + 1), len);
	if (name_len != len || memcmp(namebuf, name, len))
		goto out;
	found = true;
	*imode_ret = btrfs_type_to_imode(filetype);
out:
	btrfs_release_path(&path);
	if (!found && !ret)
		ret = -ENOENT;
	return ret;
}

static int find_file_type_dir_item(struct btrfs_root *root, u64 ino, u64 dirid,
				   const char *name, u32 name_len,
				   u32 *imode_ret)
{
	struct btrfs_path path;
	struct btrfs_key key;
	struct btrfs_key location;
	struct btrfs_dir_item *di;
	char namebuf[BTRFS_NAME_LEN] = {0};
	bool found = false;
	unsigned long cur;
	unsigned long end;
	u8 filetype;
	u32 len;
	int ret;

	btrfs_init_path(&path);
	key.objectid = dirid;
	key.offset = btrfs_name_hash(name, name_len);
	key.type = BTRFS_DIR_INDEX_KEY;

	ret = btrfs_search_slot(NULL, root, &key, &path, 0, 0);
	if (ret > 0) {
		ret = -ENOENT;
		goto out;
	}
	if (ret < 0)
		goto out;

	cur = btrfs_item_ptr_offset(path.nodes[0], path.slots[0]);
	end = cur + btrfs_item_size(path.nodes[0], path.slots[0]);
	while (cur < end) {
		di = (struct btrfs_dir_item *)cur;
		cur += btrfs_dir_name_len(path.nodes[0], di) + sizeof(*di);

		btrfs_dir_item_key_to_cpu(path.nodes[0], di, &location);
		/* Various basic check */
		if (location.objectid != ino ||
		    location.type != BTRFS_INODE_ITEM_KEY ||
		    location.offset != 0)
			continue;
		filetype = btrfs_dir_type(path.nodes[0], di);
		if (filetype >= BTRFS_FT_MAX || filetype == BTRFS_FT_UNKNOWN)
			continue;
		len = min_t(u32, BTRFS_NAME_LEN,
			    btrfs_item_size(path.nodes[0], path.slots[0]) -
			    sizeof(*di));
		len = min_t(u32, len, btrfs_dir_name_len(path.nodes[0], di));
		read_extent_buffer(path.nodes[0], namebuf,
				   (unsigned long)(di + 1), len);
		if (name_len != len || memcmp(namebuf, name, len))
			continue;
		*imode_ret = btrfs_type_to_imode(filetype);
		found = true;
		goto out;
	}
out:
	btrfs_release_path(&path);
	if (!found && !ret)
		ret = -ENOENT;
	return ret;
}

static int find_file_type(struct btrfs_root *root, u64 ino, u64 dirid,
			  u64 index, const char *name, u32 name_len,
			  u32 *imode_ret)
{
	int ret;
	ret = find_file_type_dir_index(root, ino, dirid, index, name, name_len,
				       imode_ret);
	if (ret == 0)
		return ret;
	return find_file_type_dir_item(root, ino, dirid, name, name_len,
				       imode_ret);
}

int detect_imode(struct btrfs_root *root, struct btrfs_path *path,
			u32 *imode_ret)
{
	struct btrfs_key key;
	struct btrfs_inode_item iitem;
	bool found = false;
	u64 ino;
	u32 imode = 0;
	int ret = 0;

	btrfs_item_key_to_cpu(path->nodes[0], &key, path->slots[0]);
	ino = key.objectid;
	read_extent_buffer(path->nodes[0], &iitem,
			btrfs_item_ptr_offset(path->nodes[0], path->slots[0]),
			sizeof(iitem));
	/* root inode */
	if (ino == BTRFS_FIRST_FREE_OBJECTID) {
		imode = S_IFDIR;
		found = true;
		goto out;
	}

	while (1) {
		struct btrfs_inode_ref *iref;
		struct extent_buffer *leaf;
		unsigned long cur;
		unsigned long end;
		char namebuf[BTRFS_NAME_LEN] = {0};
		u64 index;
		u32 namelen;
		int slot;

		ret = btrfs_next_item(root, path);
		if (ret > 0) {
			/* falls back to rdev check */
			ret = 0;
			goto out;
		}
		if (ret < 0)
			goto out;
		leaf = path->nodes[0];
		slot = path->slots[0];
		btrfs_item_key_to_cpu(leaf, &key, slot);
		if (key.objectid != ino)
			goto out;

		/*
		 * We ignore some types to make life easier:
		 * - XATTR
		 *   Both REG and DIR can have xattr, so not useful
		 */
		switch (key.type) {
		case BTRFS_INODE_REF_KEY:
			/* The most accurate way to determine filetype */
			cur = btrfs_item_ptr_offset(leaf, slot);
			end = cur + btrfs_item_size(leaf, slot);
			while (cur < end) {
				iref = (struct btrfs_inode_ref *)cur;
				namelen = min_t(u32, end - cur - sizeof(&iref),
					btrfs_inode_ref_name_len(leaf, iref));
				index = btrfs_inode_ref_index(leaf, iref);
				read_extent_buffer(leaf, namebuf,
					(unsigned long)(iref + 1), namelen);
				ret = find_file_type(root, ino, key.offset,
						index, namebuf, namelen,
						&imode);
				if (ret == 0) {
					found = true;
					goto out;
				}
				cur += sizeof(*iref) + namelen;
			}
			break;
		case BTRFS_DIR_ITEM_KEY:
		case BTRFS_DIR_INDEX_KEY:
			imode = S_IFDIR;
			found = true;
			goto out;
		case BTRFS_EXTENT_DATA_KEY:
			/*
			 * Both REG and LINK could have EXTENT_DATA.
			 * We just fall back to REG as user can inspect the
			 * content.
			 */
			imode = S_IFREG;
			found = true;
			goto out;
		}
	}

out:
	/*
	 * Both CHR and BLK uses rdev, no way to distinguish them, so fall back
	 * to BLK. But either way it doesn't really matter, as CHR/BLK on btrfs
	 * should be pretty rare, and no real data will be lost.
	 */
	if (!found && btrfs_stack_inode_rdev(&iitem) != 0) {
		imode = S_IFBLK;
		found = true;
	}

	if (found) {
		ret = 0;
		*imode_ret = (imode | 0700);
	} else {
		ret = -ENOENT;
	}
	return ret;
}

/*
 * Reset the inode mode of the inode specified by @path.
 *
 * Caller should ensure the @path is pointing to an INODE_ITEM and root is tree
 * root. Repair imode for other trees is not supported yet.
 *
 * Return 0 if repair is successful.
 * Return <0 if error happens.
 */
int repair_imode_common(struct btrfs_root *root, struct btrfs_path *path)
{
	struct btrfs_trans_handle *trans;
	struct btrfs_key key;
	u32 imode;
	int ret;

	btrfs_item_key_to_cpu(path->nodes[0], &key, path->slots[0]);
	ASSERT(key.type == BTRFS_INODE_ITEM_KEY);
	if (root->objectid == BTRFS_ROOT_TREE_OBJECTID) {
		/* In root tree we only have two possible imode */
		if (key.objectid == BTRFS_ROOT_TREE_OBJECTID)
			imode = S_IFDIR | 0755;
		else
			imode = S_IFREG | 0600;
	} else {
		ret = detect_imode(root, path, &imode);
		if (ret < 0)
			return ret;
	}

	trans = btrfs_start_transaction(root, 1);
	if (IS_ERR(trans)) {
		ret = PTR_ERR(trans);
		errno = -ret;
		error_msg(ERROR_MSG_START_TRANS, "%m");
		return ret;
	}
	btrfs_release_path(path);

	ret = reset_imode(trans, root, path, key.objectid, imode);
	if (ret < 0)
		goto abort;
	ret = btrfs_commit_transaction(trans, root);
	if (!ret)
		printf("reset mode for inode %llu root %llu\n",
			key.objectid, root->root_key.objectid);
	return ret;
abort:
	btrfs_abort_transaction(trans, ret);
 	return ret;
}

/*
 * For free space inodes, we can't call check_inode_item() as free space
 * cache inode doesn't have INODE_REF.
 * We just check its inode mode.
 */
int check_repair_free_space_inode(struct btrfs_path *path)
{
	struct btrfs_inode_item *iitem;
	struct btrfs_key key;
	u32 mode;
	int ret = 0;

	btrfs_item_key_to_cpu(path->nodes[0], &key, path->slots[0]);
	ASSERT(key.type == BTRFS_INODE_ITEM_KEY && is_fstree(key.objectid));
	iitem = btrfs_item_ptr(path->nodes[0], path->slots[0],
			       struct btrfs_inode_item);
	mode = btrfs_inode_mode(path->nodes[0], iitem);
	if (mode != FREE_SPACE_CACHE_INODE_MODE) {
		error(
	"free space cache inode %llu has invalid mode: has 0%o expect 0%o",
			key.objectid, mode, FREE_SPACE_CACHE_INODE_MODE);
		ret = -EUCLEAN;
		if (opt_check_repair) {
			ret = repair_imode_common(gfs_info->tree_root, path);
			if (ret < 0)
				return ret;
			return ret;
		}
	}
	return ret;
}

int recow_extent_buffer(struct btrfs_root *root, struct extent_buffer *eb)
{
	struct btrfs_path path;
	struct btrfs_trans_handle *trans;
	struct btrfs_key key;
	int ret;

	printf("Recowing metadata block %llu\n", eb->start);
	key.objectid = btrfs_header_owner(eb);
	key.type = BTRFS_ROOT_ITEM_KEY;
	key.offset = (u64)-1;

	root = btrfs_read_fs_root(gfs_info, &key);
	if (IS_ERR(root)) {
		fprintf(stderr, "Couldn't find owner root %llu\n",
			key.objectid);
		return PTR_ERR(root);
	}

	trans = btrfs_start_transaction(root, 1);
	if (IS_ERR(trans))
		return PTR_ERR(trans);

	btrfs_init_path(&path);
	path.lowest_level = btrfs_header_level(eb);
	if (path.lowest_level)
		btrfs_node_key_to_cpu(eb, &key, 0);
	else
		btrfs_item_key_to_cpu(eb, &key, 0);

	ret = btrfs_search_slot(trans, root, &key, &path, 0, 1);
	btrfs_commit_transaction(trans, root);
	btrfs_release_path(&path);
	return ret;
}

/*
 * Try to get correct extent item generation.
 *
 * Return 0 if we get a correct generation.
 * Return <0 if we failed to get one.
 */
int get_extent_item_generation(u64 bytenr, u64 *gen_ret)
{
	struct btrfs_root *root = btrfs_extent_root(gfs_info, bytenr);
	struct btrfs_extent_item *ei;
	struct btrfs_path path;
	struct btrfs_key key;
	int ret;

	key.objectid = bytenr;
	key.type = BTRFS_METADATA_ITEM_KEY;
	key.offset = (u64)-1;

	btrfs_init_path(&path);
	ret = btrfs_search_slot(NULL, root, &key, &path, 0, 0);
	/* Not possible */
	if (ret == 0)
		ret = -EUCLEAN;
	if (ret < 0)
		goto out;
	ret = btrfs_previous_extent_item(root, &path, bytenr);
	if (ret > 0)
		ret = -ENOENT;
	if (ret < 0)
		goto out;

	ei = btrfs_item_ptr(path.nodes[0], path.slots[0], struct btrfs_extent_item);

	if (btrfs_extent_flags(path.nodes[0], ei) &
	    BTRFS_EXTENT_FLAG_TREE_BLOCK) {
		struct extent_buffer *eb;

		eb = read_tree_block(gfs_info, bytenr, 0);
		if (extent_buffer_uptodate(eb)) {
			*gen_ret = btrfs_header_generation(eb);
			ret = 0;
		} else {
			ret = -EIO;
		}
		free_extent_buffer(eb);
	} else {
		/*
		 * TODO: Grab proper data generation for data extents.
		 * But this is not an urgent objective, as we can still
		 * use transaction id as fall back
		 */
		ret = -ENOTSUP;
	}
out:
	btrfs_release_path(&path);
	return ret;
}

int repair_dev_item_bytes_used(struct btrfs_fs_info *fs_info,
			       u64 devid, u64 bytes_used_expected)
{
	struct btrfs_trans_handle *trans;
	struct btrfs_device *device;
	int ret;

	device = btrfs_find_device_by_devid(fs_info->fs_devices, devid, 0);
	if (!device) {
		error("failed to find device with devid %llu", devid);
		return -ENOENT;
	}

	/* Bytes_used matches, not what we can repair */
	if (device->bytes_used == bytes_used_expected)
		return -ENOTSUP;

	/*
	 * We have to set the device bytes_used right now, before starting a
	 * new transaction, as it may allocate a new chunk and modify
	 * device->bytes_used.
	 */
	device->bytes_used = bytes_used_expected;
	trans = btrfs_start_transaction(fs_info->chunk_root, 1);
	if (IS_ERR(trans)) {
		ret = PTR_ERR(trans);
		errno = -ret;
		error_msg(ERROR_MSG_START_TRANS, "%m");
		return ret;
	}

	/* Manually update the device item in chunk tree */
	ret = btrfs_update_device(trans, device);
	if (ret < 0) {
		errno = -ret;
		error("failed to update device item for devid %llu: %m", devid);
		goto error;
	}

	/*
	 * Commit transaction not only to save the above change but also update
	 * the device item in super block.
	 */
	ret = btrfs_commit_transaction(trans, fs_info->chunk_root);
	if (ret < 0) {
		errno = -ret;
		error_msg(ERROR_MSG_START_TRANS, "%m");
	} else {
		printf("reset devid %llu bytes_used to %llu\n", devid,
		       device->bytes_used);
	}
	return ret;
error:
	btrfs_abort_transaction(trans, ret);
	return ret;
}

static int populate_csum(struct btrfs_trans_handle *trans,
			 struct btrfs_root *csum_root, char *buf, u64 start,
			 u64 len)
{
	u64 offset = 0;
	u64 sectorsize;
	int ret = 0;

	while (offset < len) {
		sectorsize = gfs_info->sectorsize;
		ret = read_data_from_disk(gfs_info, buf, start + offset,
					  &sectorsize, 0);
		if (ret)
			break;
		ret = btrfs_csum_file_block(trans, start + len, start + offset,
					    buf, sectorsize);
		if (ret)
			break;
		offset += sectorsize;
	}
	return ret;
}

static int fill_csum_tree_from_one_fs_root(struct btrfs_trans_handle *trans,
					   struct btrfs_root *cur_root)
{
	struct btrfs_root *csum_root;
	struct btrfs_path path;
	struct btrfs_key key;
	struct extent_buffer *node;
	struct btrfs_file_extent_item *fi;
	char *buf = NULL;
	u64 skip_ino = 0;
	u64 start = 0;
	u64 len = 0;
	int slot = 0;
	int ret = 0;

	buf = malloc(gfs_info->sectorsize);
	if (!buf)
		return -ENOMEM;

	btrfs_init_path(&path);
	key.objectid = 0;
	key.offset = 0;
	key.type = 0;
	ret = btrfs_search_slot(NULL, cur_root, &key, &path, 0, 0);
	if (ret < 0)
		goto out;
	/* Iterate all regular file extents and fill its csum */
	while (1) {
		u8 type;

		btrfs_item_key_to_cpu(path.nodes[0], &key, path.slots[0]);

		if (key.type != BTRFS_EXTENT_DATA_KEY &&
		    key.type != BTRFS_INODE_ITEM_KEY)
			goto next;

		/* This item belongs to an inode with NODATASUM, skip it */
		if (key.objectid == skip_ino)
			goto next;

		if (key.type == BTRFS_INODE_ITEM_KEY) {
			struct btrfs_inode_item *ii;

			ii = btrfs_item_ptr(path.nodes[0], path.slots[0],
					    struct btrfs_inode_item);
			/* Check if the inode has NODATASUM flag */
			if (btrfs_inode_flags(path.nodes[0], ii) & BTRFS_INODE_NODATASUM)
				skip_ino = key.objectid;
			goto next;
		}
		node = path.nodes[0];
		slot = path.slots[0];
		fi = btrfs_item_ptr(node, slot, struct btrfs_file_extent_item);
		type = btrfs_file_extent_type(node, fi);

		/* Skip inline extents */
		if (type == BTRFS_FILE_EXTENT_INLINE)
			goto next;

		start = btrfs_file_extent_disk_bytenr(node, fi);
		/* Skip holes */
		if (start == 0)
			goto next;
		/*
		 * Always generate the csum for the whole preallocated/regular
		 * first, then remove the csum for preallocated range.
		 *
		 * This is to handle holes on regular extents like:
		 * xfs_io -f -c "pwrite 0 8k" -c "sync" -c "punch 0 4k".
		 *
		 * This behavior will cost extra IO/CPU time, but there is
		 * not other way to ensure the correctness.
		 */
		csum_root = btrfs_csum_root(gfs_info, start);
		len = btrfs_file_extent_disk_num_bytes(node, fi);
		ret = populate_csum(trans, csum_root, buf, start, len);
		if (ret == -EEXIST)
			ret = 0;
		if (ret < 0)
			goto out;

		/* Delete the csum for the preallocated range */
		if (type == BTRFS_FILE_EXTENT_PREALLOC) {
			start += btrfs_file_extent_offset(node, fi);
			len = btrfs_file_extent_num_bytes(node, fi);
			ret = btrfs_del_csums(trans, start, len);
			if (ret < 0)
				goto out;
		}
next:
		/*
		 * TODO: if next leaf is corrupted, jump to nearest next valid
		 * leaf.
		 */
		ret = btrfs_next_item(cur_root, &path);
		if (ret < 0)
			goto out;
		if (ret > 0) {
			ret = 0;
			goto out;
		}
	}

out:
	btrfs_release_path(&path);
	free(buf);
	return ret;
}

static int fill_csum_tree_from_fs(struct btrfs_trans_handle *trans)
{
	struct btrfs_path path;
	struct btrfs_root *tree_root = gfs_info->tree_root;
	struct btrfs_root *cur_root;
	struct extent_buffer *node;
	struct btrfs_key key;
	int slot = 0;
	int ret = 0;

	btrfs_init_path(&path);
	key.objectid = BTRFS_FS_TREE_OBJECTID;
	key.offset = 0;
	key.type = BTRFS_ROOT_ITEM_KEY;
	ret = btrfs_search_slot(NULL, tree_root, &key, &path, 0, 0);
	if (ret < 0)
		goto out;
	if (ret > 0) {
		ret = -ENOENT;
		goto out;
	}

	while (1) {
		node = path.nodes[0];
		slot = path.slots[0];
		btrfs_item_key_to_cpu(node, &key, slot);
		if (key.objectid > BTRFS_LAST_FREE_OBJECTID)
			goto out;
		if (key.type != BTRFS_ROOT_ITEM_KEY)
			goto next;
		if (!is_fstree(key.objectid))
			goto next;
		key.offset = (u64)-1;

		cur_root = btrfs_read_fs_root(gfs_info, &key);
		if (IS_ERR(cur_root) || !cur_root) {
			fprintf(stderr, "Fail to read fs/subvol tree: %lld\n",
				key.objectid);
			goto out;
		}
		ret = fill_csum_tree_from_one_fs_root(trans, cur_root);
		if (ret < 0)
			goto out;
next:
		ret = btrfs_next_item(tree_root, &path);
		if (ret > 0) {
			ret = 0;
			goto out;
		}
		if (ret < 0)
			goto out;
	}

out:
	btrfs_release_path(&path);
	return ret;
}

static int remove_csum_for_file_extent(u64 ino, u64 offset, u64 rootid, void *ctx)
{
	struct btrfs_trans_handle *trans = (struct btrfs_trans_handle *)ctx;
	struct btrfs_fs_info *fs_info = trans->fs_info;
	struct btrfs_file_extent_item *fi;
	struct btrfs_inode_item *ii;
	struct btrfs_path path = {};
	struct btrfs_key key;
	struct btrfs_root *root;
	bool nocsum = false;
	u8 type;
	u64 disk_bytenr;
	u64 disk_len;
	int ret = 0;

	key.objectid = rootid;
	key.type = BTRFS_ROOT_ITEM_KEY;
	key.offset = (u64)-1;
	root = btrfs_read_fs_root(fs_info, &key);
	if (IS_ERR(root)) {
		ret = PTR_ERR(root);
		goto out;
	}

	/* Check if the inode has NODATASUM flag */
	key.objectid = ino;
	key.type = BTRFS_INODE_ITEM_KEY;
	key.offset = 0;
	ret = btrfs_search_slot(NULL, root, &key, &path, 0, 0);
	if (ret > 0)
		ret = -ENOENT;
	if (ret < 0)
		goto out;

	ii = btrfs_item_ptr(path.nodes[0], path.slots[0],
			    struct btrfs_inode_item);
	if (btrfs_inode_flags(path.nodes[0], ii) & BTRFS_INODE_NODATASUM)
		nocsum = true;

	btrfs_release_path(&path);

	/* Check the file extent item and delete csum if needed */
	key.objectid = ino;
	key.type = BTRFS_EXTENT_DATA_KEY;
	key.offset = offset;
	ret = btrfs_search_slot(NULL, root, &key, &path, 0, 0);
	if (ret > 0)
		ret = -ENOENT;
	if (ret < 0)
		goto out;
	fi = btrfs_item_ptr(path.nodes[0], path.slots[0],
			    struct btrfs_file_extent_item);
	type = btrfs_file_extent_type(path.nodes[0], fi);

	if (btrfs_file_extent_disk_bytenr(path.nodes[0], fi) == 0)
		goto out;

	/* Compressed extent should have csum, skip it */
	if (btrfs_file_extent_compression(path.nodes[0], fi) !=
	    BTRFS_COMPRESS_NONE)
		goto out;
	/*
	 * We only want to delete the csum range if the inode has NODATASUM
	 * flag or it's a preallocated extent.
	 */
	if (!(nocsum || type == BTRFS_FILE_EXTENT_PREALLOC))
		goto out;

	/* If NODATASUM, we need to remove all csum for the extent */
	if (nocsum) {
		disk_bytenr = btrfs_file_extent_disk_bytenr(path.nodes[0], fi);
		disk_len = btrfs_file_extent_disk_num_bytes(path.nodes[0], fi);
	} else {
		disk_bytenr = btrfs_file_extent_disk_bytenr(path.nodes[0], fi) +
			      btrfs_file_extent_offset(path.nodes[0], fi);
		disk_len = btrfs_file_extent_num_bytes(path.nodes[0], fi);
	}
	btrfs_release_path(&path);

	/* Now delete the csum for the preallocated or nodatasum range */
	ret = btrfs_del_csums(trans, disk_bytenr, disk_len);
out:
	btrfs_release_path(&path);
	return ret;
}

static int fill_csum_tree_from_extent(struct btrfs_trans_handle *trans,
				      struct btrfs_root *extent_root)
{
	struct btrfs_root *csum_root;
	struct btrfs_path path;
	struct btrfs_extent_item *ei;
	struct extent_buffer *leaf;
	char *buf;
	struct btrfs_key key;
	int ret;

	btrfs_init_path(&path);
	key.objectid = 0;
	key.type = BTRFS_EXTENT_ITEM_KEY;
	key.offset = 0;
	ret = btrfs_search_slot(NULL, extent_root, &key, &path, 0, 0);
	if (ret < 0) {
		btrfs_release_path(&path);
		return ret;
	}

	buf = malloc(gfs_info->sectorsize);
	if (!buf) {
		btrfs_release_path(&path);
		return -ENOMEM;
	}

	while (1) {
		if (path.slots[0] >= btrfs_header_nritems(path.nodes[0])) {
			ret = btrfs_next_leaf(extent_root, &path);
			if (ret < 0)
				break;
			if (ret) {
				ret = 0;
				break;
			}
		}
		leaf = path.nodes[0];

		btrfs_item_key_to_cpu(leaf, &key, path.slots[0]);
		if (key.type != BTRFS_EXTENT_ITEM_KEY) {
			path.slots[0]++;
			continue;
		}

		ei = btrfs_item_ptr(leaf, path.slots[0],
				    struct btrfs_extent_item);
		if (!(btrfs_extent_flags(leaf, ei) &
		      BTRFS_EXTENT_FLAG_DATA)) {
			path.slots[0]++;
			continue;
		}
		/*
		 * Generate the datasum unconditionally first.
		 *
		 * This will generate csum for preallocated extents, but that
		 * will be later deleted.
		 *
		 * This is to address cases like this:
		 *  fallocate 0 8K
		 *  pwrite 0 4k
		 *  sync
		 *  punch 0 4k
		 *
		 * Above case we will have csum for [0, 4K) and that's valid.
		 */
		csum_root = btrfs_csum_root(gfs_info, key.objectid);
		ret = populate_csum(trans, csum_root, buf, key.objectid,
				    key.offset);
		if (ret < 0)
			break;
		ret = iterate_extent_inodes(trans->fs_info, key.objectid, 0, 0,
					    remove_csum_for_file_extent, trans);
		if (ret)
			break;
		path.slots[0]++;
	}

	btrfs_release_path(&path);
	free(buf);
	return ret;
}

/*
 * Recalculate the csum and put it into the csum tree.
 *
 * @search_fs_tree:	How to get the data extent item.
 *			If true, iterate all fs roots to get all
 *			extent data (which can be slow).
 *			Otherwise, search extent tree for extent data.
 */
int fill_csum_tree(struct btrfs_trans_handle *trans, bool search_fs_tree)
{
	struct btrfs_root *root;
	struct rb_node *n;
	int ret;

	if (search_fs_tree)
		return fill_csum_tree_from_fs(trans);

	root = btrfs_extent_root(gfs_info, 0);
	while (1) {
		ret = fill_csum_tree_from_extent(trans, root);
		if (ret)
			break;
		n = rb_next(&root->rb_node);
		if (!n)
			break;
		root = rb_entry(n, struct btrfs_root, rb_node);
		if (root->root_key.objectid != BTRFS_EXTENT_TREE_OBJECTID)
			break;
	}
	return ret;
}

static int get_num_devs_in_chunk_tree(struct btrfs_fs_info *fs_info)
{
	struct btrfs_root *chunk_root = fs_info->chunk_root;
	struct btrfs_path path = { 0 };
	struct btrfs_key key = { 0 };
	int found_devs = 0;
	int ret;

	ret = btrfs_search_slot(NULL, chunk_root, &key, &path, 0, 0);
	if (ret < 0)
		return ret;

	/* We should be the first slot, and chunk tree should not be empty*/
	ASSERT(path.slots[0] == 0 && btrfs_header_nritems(path.nodes[0]));

	btrfs_item_key_to_cpu(path.nodes[0], &key, path.slots[0]);

	while (key.objectid == BTRFS_DEV_ITEMS_OBJECTID &&
	       key.type == BTRFS_DEV_ITEM_KEY) {
		found_devs++;

		ret = btrfs_next_item(chunk_root, &path);
		if (ret < 0)
			break;

		/*
		 * This should not happen, as we should have CHUNK items after
		 * DEV items, but since we're only to get the num devices, no
		 * need to bother that problem.
		 */
		if (ret > 0) {
			ret = 0;
			break;
		}
		btrfs_item_key_to_cpu(path.nodes[0], &key, path.slots[0]);
	}
	btrfs_release_path(&path);
	if (ret < 0)
		return ret;
	return found_devs;
}

int check_and_repair_super_num_devs(struct btrfs_fs_info *fs_info)
{
	int found_devs;
	int ret;

	ret = get_num_devs_in_chunk_tree(fs_info);
	if (ret < 0)
		return ret;

	found_devs = ret;

	if (found_devs == btrfs_super_num_devices(fs_info->super_copy))
		return 0;

	/* Now the found devs in chunk tree mismatch with super block */
	error("super num devices mismatch, have %llu expect %u",
	      btrfs_super_num_devices(fs_info->super_copy),
	      found_devs);

	if (!opt_check_repair)
		return -EUCLEAN;

	/*
	 * Repair is simple, reset the super block value and write back all the
	 * super blocks. Do not use transaction for that.
	 */
	btrfs_set_super_num_devices(fs_info->super_copy, found_devs);
	ret = write_all_supers(fs_info);
	if (ret < 0) {
		errno = -ret;
		error("failed to write super blocks: %m");
		return ret;
	}
	printf("Successfully reset super num devices to %u\n", found_devs);
	return 0;
}
