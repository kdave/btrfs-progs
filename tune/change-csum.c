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
#include "kernel-shared/ctree.h"
#include "kernel-shared/disk-io.h"
#include "kernel-shared/volumes.h"
#include "kernel-shared/file-item.h"
#include "kernel-shared/extent_io.h"
#include "kernel-shared/transaction.h"
#include "common/messages.h"
#include "common/internal.h"
#include "common/utils.h"
#include "tune/tune.h"

static int check_csum_change_requreiment(struct btrfs_fs_info *fs_info)
{
	struct btrfs_root *tree_root = fs_info->tree_root;
	struct btrfs_root *dev_root = fs_info->dev_root;
	struct btrfs_path path = { 0 };
	struct btrfs_key key;
	int ret;

	if (btrfs_super_log_root(fs_info->super_copy)) {
		error("dirty log tree detected, please replay the log or zero it.");
		return -EINVAL;
	}
	if (btrfs_fs_incompat(fs_info, EXTENT_TREE_V2)) {
		error("no csum change support for extent-tree-v2 feature yet.");
		return -EOPNOTSUPP;
	}
	if (btrfs_super_flags(fs_info->super_copy) &
	    (BTRFS_SUPER_FLAG_CHANGING_DATA_CSUM |
	     BTRFS_SUPER_FLAG_CHANGING_META_CSUM)) {
		error("resume from half converted status is not yet supported");
		return -EOPNOTSUPP;
	}
	key.objectid = BTRFS_BALANCE_OBJECTID;
	key.type = BTRFS_TEMPORARY_ITEM_KEY;
	key.offset = 0;
	ret = btrfs_search_slot(NULL, tree_root, &key, &path, 0, 0);
	btrfs_release_path(&path);
	if (ret < 0) {
		errno = -ret;
		error("failed to check the balance status: %m");
		return ret;
	}
	if (ret == 0) {
		error("running balance detected, please finish or cancel it.");
		return -EINVAL;
	}

	key.objectid = 0;
	key.type = BTRFS_DEV_REPLACE_KEY;
	key.offset = 0;
	ret = btrfs_search_slot(NULL, dev_root, &key, &path, 0, 0);
	btrfs_release_path(&path);
	if (ret < 0) {
		errno = -ret;
		error("failed to check the dev-reaplce status: %m");
		return ret;
	}
	if (ret == 0) {
		error("running dev-replace detected, please finish or cancel it.");
		return -EINVAL;
	}
	return 0;
}

static int get_last_csum_bytenr(struct btrfs_fs_info *fs_info, u64 *result)
{
	struct btrfs_root *csum_root = btrfs_csum_root(fs_info, 0);
	struct btrfs_path path = { 0 };
	struct btrfs_key key;
	int ret;

	key.objectid = BTRFS_EXTENT_CSUM_OBJECTID;
	key.type = BTRFS_EXTENT_CSUM_KEY;
	key.offset = (u64)-1;

	ret = btrfs_search_slot(NULL, csum_root, &key, &path, 0, 0);
	if (ret < 0)
		return ret;
	assert(ret > 0);
	ret = btrfs_previous_item(csum_root, &path, BTRFS_EXTENT_CSUM_OBJECTID,
				  BTRFS_EXTENT_CSUM_KEY);
	if (ret < 0)
		return ret;
	/*
	 * Emptry csum tree, set last csum byte to 0 so we can skip new data
	 * csum generation.
	 */
	if (ret > 0) {
		*result = 0;
		btrfs_release_path(&path);
		return 0;
	}
	btrfs_item_key_to_cpu(path.nodes[0], &key, path.slots[0]);
	*result = key.offset + btrfs_item_size(path.nodes[0], path.slots[0]) /
			       fs_info->csum_size * fs_info->sectorsize;
	btrfs_release_path(&path);
	return 0;
}

static int read_verify_one_data_sector(struct btrfs_fs_info *fs_info,
				       u64 logical, void *data_buf,
				       const void *old_csums)
{
	const u32 sectorsize = fs_info->sectorsize;
	int num_copies = btrfs_num_copies(fs_info, logical, sectorsize);
	bool found_good = false;

	for (int mirror = 1; mirror <= num_copies; mirror++) {
		u8 csum_has[BTRFS_CSUM_SIZE];
		u64 readlen = sectorsize;
		int ret;

		ret = read_data_from_disk(fs_info, data_buf, logical, &readlen,
					  mirror);
		if (ret < 0) {
			errno = -ret;
			error("failed to read logical %llu: %m", logical);
			continue;
		}
		btrfs_csum_data(fs_info, fs_info->csum_type, data_buf, csum_has,
				sectorsize);
		if (memcmp(csum_has, old_csums, fs_info->csum_size) == 0) {
			found_good = true;
			break;
		} else {
			char found[BTRFS_CSUM_STRING_LEN];
			char want[BTRFS_CSUM_STRING_LEN];

			btrfs_format_csum(fs_info->csum_type, old_csums, want);
			btrfs_format_csum(fs_info->csum_type, csum_has, found);
			error("csum mismatch for logical %llu mirror %u, has %s expected %s",
				logical, mirror, found, want);
		}
	}
	if (!found_good)
		return -EIO;
	return 0;
}

static int generate_new_csum_range(struct btrfs_trans_handle *trans,
				   u64 logical, u64 length, u16 new_csum_type,
				   const void *old_csums)
{
	struct btrfs_fs_info *fs_info = trans->fs_info;
	const u32 sectorsize = fs_info->sectorsize;
	int ret = 0;
	void *buf;

	buf = malloc(fs_info->sectorsize);
	if (!buf)
		return -ENOMEM;

	for (u64 cur = logical; cur < logical + length; cur += sectorsize) {
		ret = read_verify_one_data_sector(fs_info, cur, buf, old_csums +
				(cur - logical) / sectorsize * fs_info->csum_size);

		if (ret < 0) {
			error("failed to recover a good copy for data at logical %llu",
			      logical);
			goto out;
		}
		/* Calculate new csum and insert it into the csum tree. */
		ret = btrfs_csum_file_block(trans, cur,
				BTRFS_CSUM_CHANGE_OBJECTID, new_csum_type, buf);
		if (ret < 0) {
			errno = -ret;
			error("failed to insert new csum for data at logical %llu: %m",
			      cur);
			goto out;
		}
	}
out:
	free(buf);
	return ret;
}

/*
 * After reading this many bytes of data, commit the current transaction.
 *
 * Only a soft cap, we can exceed the threshold if hitting a large enough csum
 * item.
 */
#define CSUM_CHANGE_BYTES_THRESHOLD	(SZ_2M)
static int generate_new_data_csums(struct btrfs_fs_info *fs_info, u16 new_csum_type)
{
	struct btrfs_root *tree_root = fs_info->tree_root;
	struct btrfs_root *csum_root = btrfs_csum_root(fs_info, 0);
	struct btrfs_trans_handle *trans;
	struct btrfs_path path = { 0 };
	struct btrfs_key key;
	const u32 new_csum_size = btrfs_csum_type_size(new_csum_type);
	void *csum_buffer;
	u64 converted_bytes = 0;
	u64 last_csum;
	u64 cur = 0;
	int ret;

	ret = get_last_csum_bytenr(fs_info, &last_csum);
	if (ret < 0) {
		errno = -ret;
		error("failed to get the last csum item: %m");
		return ret;
	}
	csum_buffer = malloc(fs_info->nodesize);
	if (!csum_buffer)
		return -ENOMEM;

	trans = btrfs_start_transaction(tree_root, 1);
	if (IS_ERR(trans)) {
		ret = PTR_ERR(trans);
		errno = -ret;
		error("failed to start transaction: %m");
		goto out;
	}
	key.objectid = BTRFS_CSUM_CHANGE_OBJECTID;
	key.type = BTRFS_TEMPORARY_ITEM_KEY;
	key.offset = new_csum_type;
	ret = btrfs_insert_empty_item(trans, tree_root, &path, &key, 0);
	btrfs_release_path(&path);
	if (ret < 0) {
		errno = -ret;
		error("failed to insert csum change item: %m");
		btrfs_abort_transaction(trans, ret);
		goto out;
	}
	btrfs_set_super_flags(fs_info->super_copy,
			      btrfs_super_flags(fs_info->super_copy) |
			      BTRFS_SUPER_FLAG_CHANGING_DATA_CSUM);
	ret = btrfs_commit_transaction(trans, tree_root);
	if (ret < 0) {
		errno = -ret;
		error("failed to commit the initial transaction: %m");
		goto out;
	}

	trans = btrfs_start_transaction(csum_root,
			CSUM_CHANGE_BYTES_THRESHOLD / fs_info->sectorsize *
			new_csum_size);
	if (IS_ERR(trans)) {
		ret = PTR_ERR(trans);
		errno = -ret;
		error("failed to start transaction: %m");
		return ret;
	}

	while (cur < last_csum) {
		u64 start;
		u64 len;
		u32 item_size;

		key.objectid = BTRFS_EXTENT_CSUM_OBJECTID;
		key.type = BTRFS_EXTENT_CSUM_KEY;
		key.offset = cur;

		ret = btrfs_search_slot(NULL, csum_root, &key, &path, 0, 0);
		if (ret < 0)
			goto out;
		if (ret > 0 && path.slots[0] >=
			       btrfs_header_nritems(path.nodes[0])) {
			ret = btrfs_next_leaf(csum_root, &path);
			if (ret > 0) {
				ret = 0;
				btrfs_release_path(&path);
				break;
			}
			if (ret < 0) {
				btrfs_release_path(&path);
				goto out;
			}
		}
		btrfs_item_key_to_cpu(path.nodes[0], &key, path.slots[0]);
		assert(key.offset >= cur);
		item_size = btrfs_item_size(path.nodes[0], path.slots[0]);

		start = key.offset;
		len = item_size / fs_info->csum_size * fs_info->sectorsize;
		read_extent_buffer(path.nodes[0], csum_buffer,
				btrfs_item_ptr_offset(path.nodes[0], path.slots[0]),
				item_size);
		btrfs_release_path(&path);

		ret = generate_new_csum_range(trans, start, len, new_csum_type,
					      csum_buffer);
		if (ret < 0)
			goto out;
		converted_bytes += len;
		if (converted_bytes >= CSUM_CHANGE_BYTES_THRESHOLD) {
			converted_bytes = 0;
			ret = btrfs_commit_transaction(trans, csum_root);
			if (ret < 0)
				goto out;
			trans = btrfs_start_transaction(csum_root,
					CSUM_CHANGE_BYTES_THRESHOLD /
					fs_info->sectorsize * new_csum_size);
			if (IS_ERR(trans)) {
				ret = PTR_ERR(trans);
				goto out;
			}
		}
		cur = start + len;
	}
	ret = btrfs_commit_transaction(trans, csum_root);
out:
	free(csum_buffer);
	return ret;
}

static int delete_old_data_csums(struct btrfs_fs_info *fs_info)
{
	struct btrfs_root *csum_root = btrfs_csum_root(fs_info, 0);
	struct btrfs_trans_handle *trans;
	struct btrfs_path path = { 0 };
	struct btrfs_key last_key;
	int ret;

	last_key.objectid = BTRFS_EXTENT_CSUM_OBJECTID;
	last_key.type = BTRFS_EXTENT_CSUM_KEY;
	last_key.offset = (u64)-1;

	trans = btrfs_start_transaction(csum_root, 1);
	if (IS_ERR(trans)) {
		ret = PTR_ERR(trans);
		errno = -ret;
		error("failed to start transaction to delete old data csums: %m");
		return ret;
	}
	while (true) {
		int start_slot;
		int nr;

		ret = btrfs_search_slot(trans, csum_root, &last_key, &path, -1, 1);

		nr = btrfs_header_nritems(path.nodes[0]);
		/* No item left (empty csum tree), exit. */
		if (!nr)
			break;
		for (start_slot = 0; start_slot < nr; start_slot++) {
			struct btrfs_key found_key;

			btrfs_item_key_to_cpu(path.nodes[0], &found_key, start_slot);
			/* Break from the for loop, we found the first old csum. */
			if (found_key.objectid == BTRFS_EXTENT_CSUM_OBJECTID)
				break;
		}
		/* No more old csum item detected, exit. */
		if (start_slot == nr)
			break;

		/* Delete items starting from @start_slot to the end. */
		ret = btrfs_del_items(trans, csum_root, &path, start_slot,
				      nr - start_slot);
		if (ret < 0) {
			errno = -ret;
			error("failed to delete items: %m");
			break;
		}
		btrfs_release_path(&path);
	}
	btrfs_release_path(&path);
	if (ret < 0)
		btrfs_abort_transaction(trans, ret);
	ret = btrfs_commit_transaction(trans, csum_root);
	if (ret < 0) {
		errno = -ret;
		error("failed to commit transaction after deleting the old data csums: %m");
	}
	return ret;
}

static int change_csum_objectids(struct btrfs_fs_info *fs_info)
{
	struct btrfs_root *csum_root = btrfs_csum_root(fs_info, 0);
	struct btrfs_trans_handle *trans;
	struct btrfs_path path = { 0 };
	struct btrfs_key last_key;
	u64 super_flags;
	int ret = 0;

	last_key.objectid = BTRFS_CSUM_CHANGE_OBJECTID;
	last_key.type = BTRFS_EXTENT_CSUM_KEY;
	last_key.offset = (u64)-1;

	trans = btrfs_start_transaction(csum_root, 1);
	if (IS_ERR(trans)) {
		ret = PTR_ERR(trans);
		errno = -ret;
		error("failed to start transaction to change csum objectids: %m");
		return ret;
	}
	while (true) {
		struct btrfs_key found_key;
		int nr;

		ret = btrfs_search_slot(trans, csum_root, &last_key, &path, 0, 1);
		if (ret < 0)
			goto out;
		assert(ret > 0);

		nr = btrfs_header_nritems(path.nodes[0]);
		/* No item left (empty csum tree), exit. */
		if (!nr)
			goto out;
		/* No more temporary csum items, all converted, exit. */
		if (path.slots[0] == 0)
			goto out;

		/* All csum items should be new csums. */
		btrfs_item_key_to_cpu(path.nodes[0], &found_key, 0);
		assert(found_key.objectid == BTRFS_CSUM_CHANGE_OBJECTID);

		/*
		 * Start changing the objectids, since EXTENT_CSUM (-10) is
		 * larger than CSUM_CHANGE (-13), we always change from the tail.
		 */
		for (int i = nr - 1; i >= 0; i--) {
			btrfs_item_key_to_cpu(path.nodes[0], &found_key, i);
			found_key.objectid = BTRFS_EXTENT_CSUM_OBJECTID;
			path.slots[0] = i;
			ret = btrfs_set_item_key_safe(csum_root, &path, &found_key);
			if (ret < 0) {
				errno = -ret;
				error("failed to set item key for data csum at logical %llu: %m",
				      found_key.offset);
				goto out;
			}
		}
		btrfs_release_path(&path);
	}
out:
	btrfs_release_path(&path);
	if (ret < 0) {
		btrfs_abort_transaction(trans, ret);
		return ret;
	}

	/*
	 * All data csum items has been changed to the new type, we can clear
	 * the superblock flag for data csum change, and go to the metadata csum
	 * change phase.
	 */
	super_flags = btrfs_super_flags(fs_info->super_copy);
	super_flags &= ~BTRFS_SUPER_FLAG_CHANGING_DATA_CSUM;
	super_flags |= BTRFS_SUPER_FLAG_CHANGING_META_CSUM;
	btrfs_set_super_flags(fs_info->super_copy, super_flags);
	ret = btrfs_commit_transaction(trans, csum_root);
	if (ret < 0) {
		errno = -ret;
		error("failed to commit transaction after changing data csum objectids: %m");
	}
	return ret;
}

static int rewrite_tree_block_csum(struct btrfs_fs_info *fs_info, u64 logical,
				   u16 new_csum_type)
{
	struct extent_buffer *eb;
	u8 result_old[BTRFS_CSUM_SIZE];
	u8 result_new[BTRFS_CSUM_SIZE];
	int ret;

	eb = alloc_dummy_extent_buffer(fs_info, logical, fs_info->nodesize);
	if (!eb)
		return -ENOMEM;

	ret = btrfs_read_extent_buffer(eb, 0, 0, NULL);
	if (ret < 0) {
		errno = -ret;
		error("failed to read tree block at logical %llu: %m", logical);
		goto out;
	}

	/* Verify the csum first. */
	btrfs_csum_data(fs_info, fs_info->csum_type, (u8 *)eb->data + BTRFS_CSUM_SIZE,
			result_old, fs_info->nodesize - BTRFS_CSUM_SIZE);
	btrfs_csum_data(fs_info, new_csum_type, (u8 *)eb->data + BTRFS_CSUM_SIZE,
			result_new, fs_info->nodesize - BTRFS_CSUM_SIZE);

	/* Matches old csum, rewrite. */
	if (memcmp_extent_buffer(eb, result_old, 0, fs_info->csum_size) == 0) {
		write_extent_buffer(eb, result_new, 0,
				    btrfs_csum_type_size(new_csum_type));
		ret = write_data_to_disk(fs_info, eb->data, eb->start,
					 fs_info->nodesize);
		if (ret < 0) {
			errno = -ret;
			error("failed to write tree block at logical %llu: %m",
			      logical);
		}
		goto out;
	}

	/* Already new csum. */
	if (memcmp_extent_buffer(eb, result_new, 0, fs_info->csum_size) == 0)
		goto out;

	/* Csum doesn't match either old or new csum type, bad tree block. */
	ret = -EIO;
	error("tree block csum mismatch at logical %llu", logical);
out:
	free_extent_buffer(eb);
	return ret;
}

static int change_meta_csums(struct btrfs_fs_info *fs_info, u32 new_csum_type)
{
	struct btrfs_root *extent_root = btrfs_extent_root(fs_info, 0);
	struct btrfs_path path = { 0 };
	struct btrfs_key key;
	int ret;

	/*
	 * Disable metadata csum checks first, as we may hit tree blocks with
	 * either old or new csums.
	 * We will manually check the meta csums here.
	 */
	fs_info->skip_csum_check = true;

	key.objectid = 0;
	key.type = 0;
	key.offset = 0;

	ret = btrfs_search_slot(NULL, extent_root, &key, &path, 0, 0);
	if (ret < 0) {
		errno = -ret;
		error("failed to get the first tree block of extent tree: %m");
		return ret;
	}
	assert(ret > 0);
	while (true) {
		btrfs_item_key_to_cpu(path.nodes[0], &key, path.slots[0]);
		if (key.type != BTRFS_EXTENT_ITEM_KEY &&
		    key.type != BTRFS_METADATA_ITEM_KEY)
			goto next;

		if (key.type == BTRFS_EXTENT_ITEM_KEY) {
			struct btrfs_extent_item *ei;
			ei = btrfs_item_ptr(path.nodes[0], path.slots[0],
					    struct btrfs_extent_item);
			if (btrfs_extent_flags(path.nodes[0], ei) &
			    BTRFS_EXTENT_FLAG_DATA)
				goto next;
		}
		ret = rewrite_tree_block_csum(fs_info, key.objectid, new_csum_type);
		if (ret < 0) {
			errno = -ret;
			error("failed to rewrite csum for tree block %llu: %m",
			      key.offset);
			goto out;
		}
next:
		ret = btrfs_next_extent_item(extent_root, &path, U64_MAX);
		if (ret < 0) {
			errno = -ret;
			error("failed to get next extent item: %m");
		}
		if (ret > 0) {
			ret = 0;
			goto out;
		}
	}
out:
	btrfs_release_path(&path);

	/*
	 * Finish the change by clearing the csum change flag, update the superblock
	 * csum type, and delete the csum change item in the fs with new csum type.
	 */
	if (ret == 0) {
		struct btrfs_root *tree_root = fs_info->tree_root;
		struct btrfs_trans_handle *trans;

		u64 super_flags = btrfs_super_flags(fs_info->super_copy);

		btrfs_set_super_csum_type(fs_info->super_copy, new_csum_type);
		super_flags &= ~(BTRFS_SUPER_FLAG_CHANGING_DATA_CSUM |
				 BTRFS_SUPER_FLAG_CHANGING_META_CSUM);
		btrfs_set_super_flags(fs_info->super_copy, super_flags);

		fs_info->csum_type = new_csum_type;
		fs_info->csum_size = btrfs_csum_type_size(new_csum_type);
		fs_info->skip_csum_check = 0;

		trans = btrfs_start_transaction(tree_root, 1);
		if (IS_ERR(trans)) {
			ret = PTR_ERR(trans);
			errno = -ret;
			error("failed to start new transaction with new csum type: %m");
			return ret;
		}
		key.objectid = BTRFS_CSUM_CHANGE_OBJECTID;
		key.type = BTRFS_TEMPORARY_ITEM_KEY;
		key.offset = new_csum_type;

		ret = btrfs_search_slot(trans, tree_root, &key, &path, -1, 1);
		if (ret > 0)
			ret = -ENOENT;
		if (ret < 0) {
			errno = -ret;
			error("failed to locate the csum change item: %m");
			btrfs_release_path(&path);
			btrfs_abort_transaction(trans, ret);
			return ret;
		}
		ret = btrfs_del_item(trans, tree_root, &path);
		if (ret < 0) {
			errno = -ret;
			error("failed to delete the csum change item: %m");
			btrfs_release_path(&path);
			btrfs_abort_transaction(trans, ret);
			return ret;
		}
		btrfs_release_path(&path);
		ret = btrfs_commit_transaction(trans, tree_root);
		if (ret < 0) {
			errno = -ret;
			error("failed to finalize the csum change: %m");
		}
	}
	return ret;
}

int btrfs_change_csum_type(struct btrfs_fs_info *fs_info, u16 new_csum_type)
{
	u16 old_csum_type = fs_info->csum_type;
	int ret;

	/* Phase 0, check conflicting features. */
	ret = check_csum_change_requreiment(fs_info);
	if (ret < 0)
		return ret;

	/*
	 * Phase 1, generate new data csums.
	 *
	 * The new data csums would have a different key objectid, and there
	 * will be a temporary item in root tree to indicate the new checksum
	 * algo.
	 */
	ret = generate_new_data_csums(fs_info, new_csum_type);
	if (ret < 0) {
		errno = -ret;
		error("failed to generate new data csums: %m");
		return ret;
	}

	/* Phase 2, delete the old data csums. */
	ret = delete_old_data_csums(fs_info);
	if (ret < 0)
		return ret;

	/* Phase 3, change the new csum key objectid */
	ret = change_csum_objectids(fs_info);
	if (ret < 0)
		return ret;

	/*
	 * Phase 4, change the csums for metadata.
	 *
	 * This has to be done in-place, as we don't have a good method
	 * like relocation in progs.
	 * Thus we have to support reading a tree block with either csum.
	 */
	ret = change_meta_csums(fs_info, new_csum_type);
	if (ret == 0)
		printf("converted csum type from %s (%u) to %s (%u)\n",
		       btrfs_super_csum_name(old_csum_type), old_csum_type,
		       btrfs_super_csum_name(new_csum_type), new_csum_type);
	return ret;
}
