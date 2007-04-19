#include <stdio.h>
#include <stdlib.h>
#include "kerncompat.h"
#include "radix-tree.h"
#include "ctree.h"
#include "disk-io.h"
#include "hash.h"
#include "transaction.h"

static struct btrfs_dir_item *insert_with_overflow(struct
						   btrfs_trans_handle *trans,
						   struct btrfs_root *root,
						   struct btrfs_path *path,
						   struct btrfs_key *cpu_key,
						   u32 data_size)
{
	int ret;
	char *ptr;
	struct btrfs_item *item;
	struct btrfs_leaf *leaf;

	ret = btrfs_insert_empty_item(trans, root, path, cpu_key, data_size);
	if (ret == -EEXIST) {
		ret = btrfs_extend_item(trans, root, path, data_size);
		BUG_ON(ret > 0);
		if (ret)
			return NULL;
	}
	BUG_ON(ret > 0);
	leaf = &path->nodes[0]->leaf;
	item = leaf->items + path->slots[0];
	ptr = btrfs_item_ptr(leaf, path->slots[0], char);
	BUG_ON(data_size > btrfs_item_size(item));
	ptr += btrfs_item_size(item) - data_size;
	return (struct btrfs_dir_item *)ptr;
}

int btrfs_insert_dir_item(struct btrfs_trans_handle *trans, struct btrfs_root
			  *root, char *name, int name_len, u64 dir,
			  struct btrfs_key *location, u8 type)
{
	int ret = 0;
	struct btrfs_path path;
	struct btrfs_dir_item *dir_item;
	char *name_ptr;
	struct btrfs_key key;
	u32 data_size;

	key.objectid = dir;
	key.flags = 0;
	btrfs_set_key_type(&key, BTRFS_DIR_ITEM_KEY);
	if (name_len == 1 && *name == '.')
		key.offset = 1;
	else if (name_len == 2 && name[0] == '.' && name[1] == '.')
		key.offset = 2;
	else
		ret = btrfs_name_hash(name, name_len, &key.offset);
	BUG_ON(ret);
	btrfs_init_path(&path);
	data_size = sizeof(*dir_item) + name_len;
	dir_item = insert_with_overflow(trans, root, &path, &key, data_size);
	if (!dir_item) {
		ret = -1;
		goto out;
	}
	btrfs_cpu_key_to_disk(&dir_item->location, location);
	btrfs_set_dir_type(dir_item, type);
	btrfs_set_dir_flags(dir_item, 0);
	btrfs_set_dir_name_len(dir_item, name_len);
	name_ptr = (char *)(dir_item + 1);
	memcpy(name_ptr, name, name_len);

	/* FIXME, use some real flag for selecting the extra index */
	if (root == root->fs_info->tree_root)
		goto out;

	btrfs_release_path(root, &path);
	btrfs_set_key_type(&key, BTRFS_DIR_INDEX_KEY);
	key.offset = location->objectid;
	dir_item = insert_with_overflow(trans, root, &path, &key, data_size);
	if (!dir_item) {
		ret = -1;
		goto out;
	}
	btrfs_cpu_key_to_disk(&dir_item->location, location);
	btrfs_set_dir_type(dir_item, type);
	btrfs_set_dir_flags(dir_item, 0);
	btrfs_set_dir_name_len(dir_item, name_len);
	name_ptr = (char *)(dir_item + 1);
	memcpy(name_ptr, name, name_len);
out:
	btrfs_release_path(root, &path);
	return ret;
}

int btrfs_lookup_dir_item(struct btrfs_trans_handle *trans, struct btrfs_root
			  *root, struct btrfs_path *path, u64 dir, char *name,
			  int name_len, int mod)
{
	int ret;
	struct btrfs_key key;
	int ins_len = mod < 0 ? -1 : 0;
	int cow = mod != 0;

	key.objectid = dir;
	key.flags = 0;
	btrfs_set_key_type(&key, BTRFS_DIR_ITEM_KEY);
	ret = btrfs_name_hash(name, name_len, &key.offset);
	BUG_ON(ret);
	ret = btrfs_search_slot(trans, root, &key, path, ins_len, cow);
	return ret;
}

int btrfs_match_dir_item_name(struct btrfs_root *root,
			      struct btrfs_path *path, char
			      *name, int name_len)
{
	struct btrfs_dir_item *dir_item;
	char *name_ptr;

	dir_item = btrfs_item_ptr(&path->nodes[0]->leaf, path->slots[0],
				  struct btrfs_dir_item);
	if (btrfs_dir_name_len(dir_item) != name_len)
		return 0;
	name_ptr = (char *)(dir_item + 1);
	if (memcmp(name_ptr, name, name_len))
		return 0;
	return 1;
}
