#include <stdio.h>
#include <stdlib.h>
#include <uuid/uuid.h>
#include "kerncompat.h"
#include "radix-tree.h"
#include "ctree.h"
#include "disk-io.h"
#include "print-tree.h"
#include "transaction.h"

int main(int ac, char **av) {
	struct btrfs_super_block super;
	struct btrfs_root *root;
	struct btrfs_path path;
	struct btrfs_key key;
	struct btrfs_root_item *ri;
	struct btrfs_leaf *leaf;
	struct btrfs_key found_key;
	char uuidbuf[37];
	int ret;
	int slot;

	if (ac != 2) {
		fprintf(stderr, "usage: %s device\n", av[0]);
		exit(1);
	}
	radix_tree_init();
	root = open_ctree(av[1], &super);
	if (!root) {
		fprintf(stderr, "unable to open %s\n", av[1]);
		exit(1);
	}
	printf("root tree\n");
	btrfs_print_tree(root->fs_info->tree_root,
			 root->fs_info->tree_root->node);
	btrfs_init_path(&path);
	key.offset = 0;
	key.objectid = 0;
	key.flags = 0;
	btrfs_set_key_type(&key, BTRFS_ROOT_ITEM_KEY);
	ret = btrfs_search_slot(NULL, root->fs_info->tree_root,
					&key, &path, 0, 0);
	BUG_ON(ret < 0);
	while(1) {
		leaf = &path.nodes[0]->leaf;
		slot = path.slots[0];
		if (slot >= btrfs_header_nritems(&leaf->header)) {
			ret = btrfs_next_leaf(root, &path);
			if (ret != 0)
				break;
			leaf = &path.nodes[0]->leaf;
			slot = path.slots[0];
		}
		btrfs_disk_key_to_cpu(&found_key,
				      &leaf->items[path.slots[0]].key);
		if (btrfs_key_type(&found_key) == BTRFS_ROOT_ITEM_KEY) {
			struct btrfs_buffer *buf;
			ri = btrfs_item_ptr(leaf, path.slots[0],
					    struct btrfs_root_item);
			buf = read_tree_block(root->fs_info->tree_root,
					      btrfs_root_blocknr(ri));
			switch(found_key.objectid) {
			case BTRFS_ROOT_TREE_OBJECTID:
				printf("root ");
				break;
			case BTRFS_EXTENT_TREE_OBJECTID:
				printf("extent tree ");
				break;
			}
			printf("tree %llu %u %llu\n",
			       (unsigned long long)found_key.objectid,
			       found_key.flags,
			       (unsigned long long)found_key.offset);
			btrfs_print_tree(root, buf);
		}
		path.slots[0]++;
	}
	btrfs_release_path(root, &path);
	printf("total blocks %llu\n",
	       (unsigned long long)btrfs_super_total_blocks(&super));
	printf("blocks used %llu\n",
	       (unsigned long long)btrfs_super_blocks_used(&super));
	uuidbuf[36] = '\0';
	uuid_unparse(super.fsid, uuidbuf);
	printf("uuid %s\n", uuidbuf);
	return 0;
}
