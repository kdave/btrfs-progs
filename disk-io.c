#define _XOPEN_SOURCE 500
#include <stdio.h>
#include <stdlib.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <unistd.h>
#include "kerncompat.h"
#include "radix-tree.h"
#include "ctree.h"
#include "disk-io.h"
#include "transaction.h"

static int allocated_blocks = 0;
int cache_max = 10000;

struct dev_lookup {
	u64 block_start;
	u64 num_blocks;
	u64 device_id;
	int fd;
};

int btrfs_insert_dev_radix(struct btrfs_root *root,
			   int fd,
			   u64 device_id,
			   u64 block_start,
			   u64 num_blocks)
{
	struct dev_lookup *lookup;
	int ret;

	lookup = malloc(sizeof(*lookup));
	if (!lookup)
		return -ENOMEM;
	lookup->block_start = block_start;
	lookup->num_blocks = num_blocks;
	lookup->fd = fd;
	lookup->device_id = device_id;
printf("inserting into dev radix %Lu %Lu\n", block_start, num_blocks);

	ret = radix_tree_insert(&root->fs_info->dev_radix, block_start +
				num_blocks - 1, lookup);
	return ret;
}

int btrfs_map_bh_to_logical(struct btrfs_root *root, struct btrfs_buffer *bh,
			     u64 logical)
{
	struct dev_lookup *lookup[2];

	int ret;

	root = root->fs_info->dev_root;
	ret = radix_tree_gang_lookup(&root->fs_info->dev_radix,
				     (void **)lookup,
				     (unsigned long)logical,
				     ARRAY_SIZE(lookup));
	if (ret == 0 || lookup[0]->block_start > logical ||
	    lookup[0]->block_start + lookup[0]->num_blocks <= logical) {
		ret = -1;
		goto out;
	}
	bh->fd = lookup[0]->fd;
	bh->dev_blocknr = logical - lookup[0]->block_start;
	ret = 0;
out:
	return ret;
}

static int check_tree_block(struct btrfs_root *root, struct btrfs_buffer *buf)
{
	if (buf->blocknr != btrfs_header_blocknr(&buf->node.header))
		BUG();
	if (memcmp(root->fs_info->disk_super->fsid, buf->node.header.fsid,
		   sizeof(buf->node.header.fsid)))
		BUG();
	return 0;
}

static int free_some_buffers(struct btrfs_root *root)
{
	struct list_head *node, *next;
	struct btrfs_buffer *b;
	if (root->fs_info->cache_size < cache_max)
		return 0;
	list_for_each_safe(node, next, &root->fs_info->cache) {
		b = list_entry(node, struct btrfs_buffer, cache);
		if (b->count == 1) {
			BUG_ON(!list_empty(&b->dirty));
			list_del_init(&b->cache);
			btrfs_block_release(root, b);
			if (root->fs_info->cache_size < cache_max)
				break;
		}
	}
	return 0;
}

struct btrfs_buffer *alloc_tree_block(struct btrfs_root *root, u64 blocknr)
{
	struct btrfs_buffer *buf;
	int ret;

	buf = malloc(sizeof(struct btrfs_buffer) + root->blocksize);
	if (!buf)
		return buf;
	allocated_blocks++;
	buf->blocknr = blocknr;
	buf->count = 2;
	INIT_LIST_HEAD(&buf->dirty);
	free_some_buffers(root);
	radix_tree_preload(GFP_KERNEL);
	ret = radix_tree_insert(&root->fs_info->cache_radix, blocknr, buf);
	radix_tree_preload_end();
	list_add_tail(&buf->cache, &root->fs_info->cache);
	root->fs_info->cache_size++;
	if (ret) {
		free(buf);
		return NULL;
	}
	return buf;
}

struct btrfs_buffer *find_tree_block(struct btrfs_root *root, u64 blocknr)
{
	struct btrfs_buffer *buf;
	buf = radix_tree_lookup(&root->fs_info->cache_radix, blocknr);
	if (buf) {
		buf->count++;
	} else {
		buf = alloc_tree_block(root, blocknr);
		if (!buf) {
			BUG();
			return NULL;
		}
	}
	return buf;
}

struct btrfs_buffer *read_tree_block(struct btrfs_root *root, u64 blocknr)
{
	struct btrfs_buffer *buf;
	int ret;
	buf = radix_tree_lookup(&root->fs_info->cache_radix, blocknr);
	if (buf) {
		buf->count++;
	} else {
		buf = alloc_tree_block(root, blocknr);
		if (!buf)
			return NULL;
		btrfs_map_bh_to_logical(root, buf, blocknr);
		ret = pread(buf->fd, &buf->node, root->blocksize,
			    buf->dev_blocknr * root->blocksize);
		if (ret != root->blocksize) {
			free(buf);
			return NULL;
		}
	}
	if (check_tree_block(root, buf))
		BUG();
	return buf;
}

int dirty_tree_block(struct btrfs_trans_handle *trans, struct btrfs_root *root,
		     struct btrfs_buffer *buf)
{
	if (!list_empty(&buf->dirty))
		return 0;
	list_add_tail(&buf->dirty, &root->fs_info->trans);
	buf->count++;
	return 0;
}

int clean_tree_block(struct btrfs_trans_handle *trans, struct btrfs_root *root,
		     struct btrfs_buffer *buf)
{
	if (!list_empty(&buf->dirty)) {
		list_del_init(&buf->dirty);
		btrfs_block_release(root, buf);
	}
	return 0;
}

int write_tree_block(struct btrfs_trans_handle *trans, struct btrfs_root *root,
		     struct btrfs_buffer *buf)
{
	int ret;

	if (buf->blocknr != btrfs_header_blocknr(&buf->node.header))
		BUG();
	btrfs_map_bh_to_logical(root, buf, buf->blocknr);
	ret = pwrite(buf->fd, &buf->node, root->blocksize,
		     buf->dev_blocknr * root->blocksize);
	if (ret != root->blocksize)
		return ret;
	return 0;
}

static int __commit_transaction(struct btrfs_trans_handle *trans, struct
				btrfs_root *root)
{
	struct btrfs_buffer *b;
	int ret = 0;
	int wret;
	while(!list_empty(&root->fs_info->trans)) {
		b = list_entry(root->fs_info->trans.next, struct btrfs_buffer,
			       dirty);
		list_del_init(&b->dirty);
		wret = write_tree_block(trans, root, b);
		if (wret)
			ret = wret;
		btrfs_block_release(root, b);
	}
	return ret;
}

static int commit_tree_roots(struct btrfs_trans_handle *trans,
			     struct btrfs_fs_info *fs_info)
{
	int ret;
	u64 old_extent_block;
	struct btrfs_root *tree_root = fs_info->tree_root;
	struct btrfs_root *extent_root = fs_info->extent_root;

	if (btrfs_super_device_root(fs_info->disk_super) !=
	    fs_info->dev_root->node->blocknr) {
		btrfs_set_super_device_root(fs_info->disk_super,
					    fs_info->dev_root->node->blocknr);
	}
	while(1) {
		old_extent_block = btrfs_root_blocknr(&extent_root->root_item);
		if (old_extent_block == extent_root->node->blocknr)
			break;
		btrfs_set_root_blocknr(&extent_root->root_item,
				       extent_root->node->blocknr);
		ret = btrfs_update_root(trans, tree_root,
					&extent_root->root_key,
					&extent_root->root_item);
		BUG_ON(ret);
	}
	return 0;
}

int btrfs_commit_transaction(struct btrfs_trans_handle *trans, struct
			     btrfs_root *root, struct btrfs_super_block *s)
{
	int ret = 0;
	struct btrfs_buffer *snap = root->commit_root;
	struct btrfs_key snap_key;

	if (root->commit_root == root->node)
		return 0;

	memcpy(&snap_key, &root->root_key, sizeof(snap_key));
	root->root_key.offset++;

	btrfs_set_root_blocknr(&root->root_item, root->node->blocknr);
	ret = btrfs_insert_root(trans, root->fs_info->tree_root,
				&root->root_key, &root->root_item);
	BUG_ON(ret);

	ret = commit_tree_roots(trans, root->fs_info);
	BUG_ON(ret);

	ret = __commit_transaction(trans, root);
	BUG_ON(ret);

	write_ctree_super(trans, root, s);
	btrfs_finish_extent_commit(trans, root->fs_info->extent_root);
	btrfs_finish_extent_commit(trans, root->fs_info->tree_root);

	root->commit_root = root->node;
	root->node->count++;
	ret = btrfs_drop_snapshot(trans, root, snap);
	BUG_ON(ret);

	ret = btrfs_del_root(trans, root->fs_info->tree_root, &snap_key);
	BUG_ON(ret);
	root->fs_info->generation = root->root_key.offset + 1;

	return ret;
}

static int __setup_root(struct btrfs_super_block *super,
			struct btrfs_root *root,
			struct btrfs_fs_info *fs_info,
			u64 objectid, int fp)
{
	root->node = NULL;
	root->commit_root = NULL;
	root->blocksize = btrfs_super_blocksize(super);
	root->ref_cows = 0;
	root->fs_info = fs_info;
	memset(&root->root_key, 0, sizeof(root->root_key));
	memset(&root->root_item, 0, sizeof(root->root_item));
	return 0;
}

static int find_and_setup_root(struct btrfs_super_block *super,
			       struct btrfs_root *tree_root,
			       struct btrfs_fs_info *fs_info,
			       u64 objectid,
			       struct btrfs_root *root, int fp)
{
	int ret;

	__setup_root(super, root, fs_info, objectid, fp);
	ret = btrfs_find_last_root(tree_root, objectid,
				   &root->root_item, &root->root_key);
	BUG_ON(ret);

	root->node = read_tree_block(root,
				     btrfs_root_blocknr(&root->root_item));
	BUG_ON(!root->node);
	return 0;
}

int btrfs_open_disk(struct btrfs_root *root, u64 device_id,
		    u64 block_start, u64 num_blocks,
		    char *filename, int name_len)
{
	char *null_filename;
	int fd;
	int ret;

	null_filename = malloc(name_len + 1);
	if (!null_filename)
		return -ENOMEM;
	memcpy(null_filename, filename, name_len);
	null_filename[name_len] = '\0';

	fd = open(null_filename, O_RDWR);
	if (fd < 0) {
		ret = -1;
		goto out;
	}
	ret = btrfs_insert_dev_radix(root, fd, device_id,
				     block_start, num_blocks);
	BUG_ON(ret);
	ret = 0;
out:
	free(null_filename);
	return ret;
}

static int read_device_info(struct btrfs_root *root)
{
	struct btrfs_path path;
	int ret;
	struct btrfs_key key;
	struct btrfs_leaf *leaf;
	struct btrfs_device_item *dev_item;
	int nritems;
	int slot;

	root = root->fs_info->dev_root;

	btrfs_init_path(&path);
	key.objectid = 0;
	key.offset = 0;
	key.flags = 0;
	btrfs_set_key_type(&key, BTRFS_DEV_ITEM_KEY);

	ret = btrfs_search_slot(NULL, root, &key, &path, 0, 0);
	leaf = &path.nodes[0]->leaf;
	nritems = btrfs_header_nritems(&leaf->header);
	while(1) {
		slot = path.slots[0];
		if (slot >= nritems) {
			ret = btrfs_next_leaf(root, &path);
			if (ret)
				break;
			leaf = &path.nodes[0]->leaf;
			nritems = btrfs_header_nritems(&leaf->header);
			slot = path.slots[0];
		}
		btrfs_disk_key_to_cpu(&key, &leaf->items[slot].key);
		if (btrfs_key_type(&key) != BTRFS_DEV_ITEM_KEY) {
			path.slots[0]++;
			continue;
		}
		dev_item = btrfs_item_ptr(leaf, slot, struct btrfs_device_item);
		if (btrfs_device_id(dev_item) !=
		    btrfs_super_device_id(root->fs_info->disk_super)) {
printf("found key %Lu %Lu\n", key.objectid, key.offset);
			ret = btrfs_open_disk(root, btrfs_device_id(dev_item),
					      key.objectid, key.offset,
					      (char *)(dev_item + 1),
					      btrfs_device_pathlen(dev_item));
			BUG_ON(ret);
		}
		path.slots[0]++;
	}
	btrfs_release_path(root, &path);
	return 0;
}

struct btrfs_root *open_ctree(char *filename, struct btrfs_super_block *super)
{
	int fp;

	fp = open(filename, O_CREAT | O_RDWR, 0600);
	if (fp < 0) {
		return NULL;
	}
	return open_ctree_fd(fp, super);
}

struct btrfs_root *open_ctree_fd(int fp, struct btrfs_super_block *super)
{
	struct btrfs_root *root = malloc(sizeof(struct btrfs_root));
	struct btrfs_root *extent_root = malloc(sizeof(struct btrfs_root));
	struct btrfs_root *tree_root = malloc(sizeof(struct btrfs_root));
	struct btrfs_root *dev_root = malloc(sizeof(struct btrfs_root));
	struct btrfs_fs_info *fs_info = malloc(sizeof(*fs_info));
	struct dev_lookup *dev_lookup;
	int ret;

	INIT_RADIX_TREE(&fs_info->cache_radix, GFP_KERNEL);
	INIT_RADIX_TREE(&fs_info->pinned_radix, GFP_KERNEL);
	INIT_RADIX_TREE(&fs_info->dev_radix, GFP_KERNEL);
	INIT_LIST_HEAD(&fs_info->trans);
	INIT_LIST_HEAD(&fs_info->cache);
	fs_info->cache_size = 0;
	fs_info->fp = fp;
	fs_info->running_transaction = NULL;
	fs_info->fs_root = root;
	fs_info->tree_root = tree_root;
	fs_info->extent_root = extent_root;
	fs_info->dev_root = dev_root;
	fs_info->last_inode_alloc = 0;
	fs_info->last_inode_alloc_dirid = 0;
	fs_info->disk_super = super;
	memset(&fs_info->current_insert, 0, sizeof(fs_info->current_insert));
	memset(&fs_info->last_insert, 0, sizeof(fs_info->last_insert));

	ret = pread(fp, super, sizeof(struct btrfs_super_block),
		     BTRFS_SUPER_INFO_OFFSET);
	if (ret == 0 || btrfs_super_root(super) == 0) {
		BUG();
		return NULL;
	}
	BUG_ON(ret < 0);
	__setup_root(super, dev_root, fs_info, BTRFS_DEV_TREE_OBJECTID, fp);

	dev_lookup = malloc(sizeof(*dev_lookup));
	dev_lookup->fd = fp;
	dev_lookup->device_id = btrfs_super_device_id(super);
	dev_lookup->block_start = btrfs_super_device_block_start(super);
	dev_lookup->num_blocks = btrfs_super_device_num_blocks(super);
	ret = radix_tree_insert(&fs_info->dev_radix,
				dev_lookup->block_start +
				dev_lookup->num_blocks - 1, dev_lookup);
	BUG_ON(ret);

	dev_root->node = read_tree_block(dev_root,
					 btrfs_super_device_root(super));

	ret = read_device_info(dev_root);
	BUG_ON(ret);

	__setup_root(super, tree_root, fs_info, BTRFS_ROOT_TREE_OBJECTID, fp);
	tree_root->node = read_tree_block(tree_root, btrfs_super_root(super));
	BUG_ON(!tree_root->node);

	ret = find_and_setup_root(super, tree_root, fs_info,
				  BTRFS_EXTENT_TREE_OBJECTID, extent_root, fp);
	BUG_ON(ret);

	ret = find_and_setup_root(super, tree_root, fs_info,
				  BTRFS_FS_TREE_OBJECTID, root, fp);
	BUG_ON(ret);

	root->commit_root = root->node;
	root->node->count++;
	root->ref_cows = 1;
	root->fs_info->generation = root->root_key.offset + 1;
	return root;
}

int write_ctree_super(struct btrfs_trans_handle *trans, struct btrfs_root
		      *root, struct btrfs_super_block *s)
{
	int ret;
	btrfs_set_super_root(s, root->fs_info->tree_root->node->blocknr);
	ret = pwrite(root->fs_info->fp, s, sizeof(*s),
		     BTRFS_SUPER_INFO_OFFSET);
	if (ret != sizeof(*s)) {
		fprintf(stderr, "failed to write new super block err %d\n", ret);
		return ret;
	}
	return 0;
}

static int drop_cache(struct btrfs_root *root)
{
	while(!list_empty(&root->fs_info->cache)) {
		struct btrfs_buffer *b = list_entry(root->fs_info->cache.next,
						    struct btrfs_buffer,
						    cache);
		list_del_init(&b->cache);
		btrfs_block_release(root, b);
	}
	return 0;
}

static int free_dev_radix(struct btrfs_fs_info *fs_info)
{
	struct dev_lookup *lookup[8];
	int ret;
	int i;
	while(1) {
		ret = radix_tree_gang_lookup(&fs_info->dev_radix,
					     (void **)lookup, 0,
					     ARRAY_SIZE(lookup));
		if (!ret)
			break;
		for (i = 0; i < ret; i++) {
			if (lookup[i]->device_id !=
			    btrfs_super_device_id(fs_info->disk_super))
				close(lookup[i]->fd);
			radix_tree_delete(&fs_info->dev_radix,
					  lookup[i]->block_start +
					  lookup[i]->num_blocks - 1);
			free(lookup[i]);
		}
	}
	return 0;
}

int close_ctree(struct btrfs_root *root, struct btrfs_super_block *s)
{
	int ret;
	struct btrfs_trans_handle *trans;

	trans = root->fs_info->running_transaction;
	btrfs_commit_transaction(trans, root, s);
	ret = commit_tree_roots(trans, root->fs_info);
	BUG_ON(ret);
	ret = __commit_transaction(trans, root);
	BUG_ON(ret);
	write_ctree_super(trans, root, s);
	drop_cache(root);
	BUG_ON(!list_empty(&root->fs_info->trans));

	free_dev_radix(root->fs_info);
	close(root->fs_info->fp);
	if (root->node)
		btrfs_block_release(root, root->node);
	if (root->fs_info->extent_root->node)
		btrfs_block_release(root->fs_info->extent_root,
				    root->fs_info->extent_root->node);
	if (root->fs_info->tree_root->node)
		btrfs_block_release(root->fs_info->tree_root,
				    root->fs_info->tree_root->node);
	if (root->fs_info->dev_root->node)
		btrfs_block_release(root->fs_info->dev_root,
				    root->fs_info->dev_root->node);
	btrfs_block_release(root, root->commit_root);
	free(root);
	printf("on close %d blocks are allocated\n", allocated_blocks);
	return 0;
}

void btrfs_block_release(struct btrfs_root *root, struct btrfs_buffer *buf)
{
	buf->count--;
	if (buf->count < 0)
		BUG();
	if (buf->count == 0) {
		BUG_ON(!list_empty(&buf->cache));
		BUG_ON(!list_empty(&buf->dirty));
		if (!radix_tree_lookup(&root->fs_info->cache_radix,
				       buf->blocknr))
			BUG();
		radix_tree_delete(&root->fs_info->cache_radix, buf->blocknr);
		memset(buf, 0, sizeof(*buf));
		free(buf);
		BUG_ON(allocated_blocks == 0);
		allocated_blocks--;
		BUG_ON(root->fs_info->cache_size == 0);
		root->fs_info->cache_size--;
	}
}

