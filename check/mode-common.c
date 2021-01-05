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

#include <time.h>
#include "kernel-shared/ctree.h"
#include "common/internal.h"
#include "common/messages.h"
#include "kernel-shared/transaction.h"
#include "common/utils.h"
#include "kernel-shared/disk-io.h"
#include "repair.h"
#include "check/mode-common.h"

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
	ret = btrfs_search_slot(NULL, gfs_info->extent_root, &key, &path, 0, 0);
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
	item_size = btrfs_item_size_nr(path.nodes[0], path.slots[0]);
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
			ret = btrfs_next_leaf(gfs_info->extent_root, &path);
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
	struct btrfs_key key;
	struct btrfs_path path;
	struct extent_buffer *leaf;
	int ret;
	size_t size;
	*found = 0;
	u64 csum_end;
	u16 csum_size = btrfs_super_csum_size(gfs_info->super_copy);

	btrfs_init_path(&path);

	key.objectid = BTRFS_EXTENT_CSUM_OBJECTID;
	key.offset = start;
	key.type = BTRFS_EXTENT_CSUM_KEY;

	ret = btrfs_search_slot(NULL, gfs_info->csum_root,
				&key, &path, 0, 0);
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
			ret = btrfs_next_leaf(gfs_info->csum_root, &path);
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

		size = btrfs_item_size_nr(leaf, path.slots[0]);
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

static int traverse_tree_blocks(struct extent_buffer *eb, int tree_root, int pin)
{
	struct extent_buffer *tmp;
	struct btrfs_root_item *ri;
	struct btrfs_key key;
	struct extent_io_tree *tree;
	u64 bytenr;
	int level = btrfs_header_level(eb);
	int nritems;
	int ret;
	int i;
	u64 end = eb->start + eb->len;

	if (pin)
		tree = &gfs_info->pinned_extents;
	else
		tree = gfs_info->excluded_extents;
	/*
	 * If we have pinned/excluded this block before, don't do it again.
	 * This can not only avoid forever loop with broken filesystem
	 * but also give us some speedups.
	 */
	if (test_range_bit(tree, eb->start, end - 1, EXTENT_DIRTY, 0))
		return 0;

	if (pin)
		btrfs_pin_extent(gfs_info, eb->start, eb->len);
	else
		set_extent_dirty(tree, eb->start, end - 1);

	nritems = btrfs_header_nritems(eb);
	for (i = 0; i < nritems; i++) {
		if (level == 0) {
			bool is_extent_root;
			btrfs_item_key_to_cpu(eb, &key, i);
			if (key.type != BTRFS_ROOT_ITEM_KEY)
				continue;
			/* Skip the extent root and reloc roots */
			if (key.objectid == BTRFS_TREE_RELOC_OBJECTID ||
			    key.objectid == BTRFS_DATA_RELOC_TREE_OBJECTID)
				continue;
			is_extent_root =
				key.objectid == BTRFS_EXTENT_TREE_OBJECTID;
			/* If pin, skip the extent root */
			if (pin && is_extent_root)
				continue;
			ri = btrfs_item_ptr(eb, i, struct btrfs_root_item);
			bytenr = btrfs_disk_root_bytenr(eb, ri);

			/*
			 * If at any point we start needing the real root we
			 * will have to build a stump root for the root we are
			 * in, but for now this doesn't actually use the root so
			 * just pass in extent_root.
			 */
			tmp = read_tree_block(gfs_info, bytenr, 0);
			if (!extent_buffer_uptodate(tmp)) {
				fprintf(stderr, "Error reading root block\n");
				return -EIO;
			}
			ret = traverse_tree_blocks(tmp, 0, pin);
			free_extent_buffer(tmp);
			if (ret)
				return ret;
		} else {
			bytenr = btrfs_node_blockptr(eb, i);

			/* If we aren't the tree root don't read the block */
			if (level == 1 && !tree_root) {
				if (pin)
					btrfs_pin_extent(gfs_info, bytenr,
							 gfs_info->nodesize);
				else
					set_extent_dirty(tree, bytenr,
							 gfs_info->nodesize);
				continue;
			}

			tmp = read_tree_block(gfs_info, bytenr, 0);
			if (!extent_buffer_uptodate(tmp)) {
				fprintf(stderr, "Error reading tree block\n");
				return -EIO;
			}
			ret = traverse_tree_blocks(tmp, tree_root, pin);
			free_extent_buffer(tmp);
			if (ret)
				return ret;
		}
	}

	return 0;
}

static int pin_down_tree_blocks(struct extent_buffer *eb, int tree_root)
{
	return traverse_tree_blocks(eb, tree_root, 1);
}

int pin_metadata_blocks(void)
{
	int ret;

	ret = pin_down_tree_blocks(gfs_info->chunk_root->node, 0);
	if (ret)
		return ret;

	return pin_down_tree_blocks(gfs_info->tree_root->node, 1);
}

static int exclude_tree_blocks(struct extent_buffer *eb, int tree_root)
{
	return traverse_tree_blocks(eb, tree_root, 0);
}

int exclude_metadata_blocks(void)
{
	int ret;
	struct extent_io_tree *excluded_extents;

	excluded_extents = malloc(sizeof(*excluded_extents));
	if (!excluded_extents)
		return -ENOMEM;
	extent_io_tree_init(excluded_extents);
	gfs_info->excluded_extents = excluded_extents;

	ret = exclude_tree_blocks(gfs_info->chunk_root->node, 0);
	if (ret)
		return ret;
	return exclude_tree_blocks(gfs_info->tree_root->node, 1);
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
		btrfs_item_size_nr(path.nodes[0], path.slots[0]) - sizeof(*di));
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
	end = cur + btrfs_item_size_nr(path.nodes[0], path.slots[0]);
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
			    btrfs_item_size_nr(path.nodes[0], path.slots[0]) -
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
			end = cur + btrfs_item_size_nr(leaf, slot);
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
		error("failed to start transaction: %m");
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
		if (repair) {
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
	struct btrfs_root *root = gfs_info->extent_root;
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
