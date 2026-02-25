/*
 * Copyright (C) 2007 Oracle.  All rights reserved.
 *
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
#include <stdlib.h>
#include <unistd.h>
#include <getopt.h>
#include <errno.h>
#include <stdbool.h>
#include <string.h>
#include <strings.h>
#include <uuid/uuid.h>
#include "kernel-shared/accessors.h"
#include "kernel-shared/uapi/btrfs.h"
#include "kernel-shared/uapi/btrfs_tree.h"
#include "kernel-shared/ctree.h"
#include "kernel-shared/disk-io.h"
#include "kernel-shared/print-tree.h"
#include "kernel-shared/extent_io.h"
#include "kernel-shared/tree-checker.h"
#include "kernel-shared/volumes.h"
#include "common/defs.h"
#include "common/extent-cache.h"
#include "common/messages.h"
#include "common/help.h"
#include "common/device-scan.h"
#include "common/string-utils.h"
#include "common/parse-utils.h"
#include "cmds/commands.h"

static void print_extents(struct extent_buffer *eb)
{
	struct btrfs_fs_info *fs_info = eb->fs_info;
	struct extent_buffer *next;
	int i;
	u32 nr;

	if (!eb)
		return;

	if (btrfs_is_leaf(eb)) {
		btrfs_print_leaf(eb);
		return;
	}

	nr = btrfs_header_nritems(eb);
	for (i = 0; i < nr; i++) {
		struct btrfs_tree_parent_check check = {
			.owner_root = btrfs_header_owner(eb),
			.transid = btrfs_node_ptr_generation(eb, i),
			.level = btrfs_header_level(eb) - 1,
		};

		next = read_tree_block(fs_info, btrfs_node_blockptr(eb, i),
				       &check);
		if (!extent_buffer_uptodate(next))
			continue;
		if (btrfs_is_leaf(next) && btrfs_header_level(eb) != 1) {
			warning(
	"eb corrupted: item %d eb level %d next level %d, skipping the rest",
				i, btrfs_header_level(next),
				btrfs_header_level(eb));
			goto out;
		}
		if (btrfs_header_level(next) != btrfs_header_level(eb) - 1) {
			warning(
	"eb corrupted: item %d eb level %d next level %d, skipping the rest",
				i, btrfs_header_level(next),
				btrfs_header_level(eb));
			goto out;
		}
		print_extents(next);
		free_extent_buffer(next);
	}

	return;

out:
	free_extent_buffer(next);
}

static void print_old_roots(struct btrfs_super_block *super)
{
	const char *extent_tree_str = "extent root";
	struct btrfs_root_backup *backup;
	int i;
	bool extent_tree_v2 = (btrfs_super_incompat_flags(super) &
		BTRFS_FEATURE_INCOMPAT_EXTENT_TREE_V2);

	if (extent_tree_v2)
		extent_tree_str = "block group root";

	for (i = 0; i < BTRFS_NUM_BACKUP_ROOTS; i++) {
		backup = super->super_roots + i;
		pr_verbose(LOG_DEFAULT, "btrfs root backup slot %d\n", i);
		pr_verbose(LOG_DEFAULT, "\ttree root gen %llu block %llu\n",
		       btrfs_backup_tree_root_gen(backup),
		       btrfs_backup_tree_root(backup));

		pr_verbose(LOG_DEFAULT, "\t\t%s gen %llu block %llu\n", extent_tree_str,
		       btrfs_backup_extent_root_gen(backup),
		       btrfs_backup_extent_root(backup));

		pr_verbose(LOG_DEFAULT, "\t\tchunk root gen %llu block %llu\n",
		       btrfs_backup_chunk_root_gen(backup),
		       btrfs_backup_chunk_root(backup));

		pr_verbose(LOG_DEFAULT, "\t\tdevice root gen %llu block %llu\n",
		       btrfs_backup_dev_root_gen(backup),
		       btrfs_backup_dev_root(backup));

		pr_verbose(LOG_DEFAULT, "\t\tcsum root gen %llu block %llu\n",
		       btrfs_backup_csum_root_gen(backup),
		       btrfs_backup_csum_root(backup));

		pr_verbose(LOG_DEFAULT, "\t\tfs root gen %llu block %llu\n",
		       btrfs_backup_fs_root_gen(backup),
		       btrfs_backup_fs_root(backup));

		pr_verbose(LOG_DEFAULT, "\t\t%llu used %llu total %llu devices\n",
		       btrfs_backup_bytes_used(backup),
		       btrfs_backup_total_bytes(backup),
		       btrfs_backup_num_devices(backup));
	}
}

static const char * const cmd_inspect_dump_tree_usage[] = {
	"btrfs inspect-internal dump-tree [options] <device> [<device> ..]",
	"Dump tree structures from a given device",
	"Dump tree structures from a given device in textual form, expand keys to human",
	"readable equivalents where possible.",
	"Note: contains file names, consider that if you're asked to send the dump",
	"for analysis.",
	"",
	OPTLINE("-e|--extents", "print only extent info: extent and device trees"),
	OPTLINE("-d|--device", "print only device info: tree root, chunk and device trees"),
	OPTLINE("-r|--roots", "print only short root node info"),
	OPTLINE("-R|--backups", "same as --roots plus print backup root info"),
	OPTLINE("-u|--uuid", "print only the uuid tree"),
	OPTLINE("-b|--block <block_num>", "print info from the specified block only can be specified multiple times"),
	OPTLINE("-t|--tree <tree_id>", "print only tree with the given id (string or number)"),
	OPTLINE("--follow", "use with -b, to show all children tree blocks of <block_num>"),
	OPTLINE("--noscan", "do not scan the devices from the filesystem, use only the listed ones"),
	OPTLINE("--bfs", "breadth-first traversal of the trees, print nodes, then leaves (default)"),
	OPTLINE("--dfs", "depth-first traversal of the trees"),
	OPTLINE("--hide-names", "hide filenames/subvolume/xattrs and other name references"),
	OPTLINE("--csum-headers", "print node checksums stored in headers (metadata)"),
	OPTLINE("--csum-items", "print checksums stored in checksum items (data)"),
	NULL
};

/*
 * Helper function to record all tree block bytenr so we don't need to put
 * all code into deep indent.
 *
 * Return >0 if we hit a duplicated bytenr (already recorded)
 * Return 0 if nothing went wrong
 * Return <0 if error happens (ENOMEM)
 *
 * For != 0 return value, all warning/error will be outputted by this function.
 */
static int dump_add_tree_block(struct cache_tree *tree, u64 bytenr)
{
	int ret;

	/*
	 * We don't really care about the size and we don't have
	 * nodesize before we open the fs, so just use 1 as size here.
	 */
	ret = add_cache_extent(tree, bytenr, 1);
	if (ret == -EEXIST) {
		warning("tree block bytenr %llu is duplicated", bytenr);
		return 1;
	}
	if (ret < 0) {
		errno = -ret;
		error("failed to record tree block bytenr %llu: %m", bytenr);
		return ret;
	}
	return ret;
}

static int check_metadata_logical(struct btrfs_fs_info *fs_info, u64 logical)
{
	struct cache_extent *ce;
	struct map_lookup *map;

	ce = search_cache_extent(&fs_info->mapping_tree.cache_tree, logical);
	if (!ce || ce->start > logical) {
		error("no chunk map found for logical %llu", logical);
		return -ENOENT;
	}
	map = container_of(ce, struct map_lookup, ce);
	if (!(map->type & BTRFS_BLOCK_GROUP_METADATA)) {
		error(
"logical %llu is not in a metadata chunk, found chunk %llu len %llu flags 0x%llx",
		      logical, ce->start, ce->size, map->type);
		return -EINVAL;
	}
	return 0;
}

/*
 * Print all tree blocks recorded.
 * All tree block bytenr record will also be freed in this function.
 *
 * Return 0 if nothing wrong happened for *each* tree blocks
 * Return <0 if anything wrong happened, and return value will be the last
 * error.
 */
static int dump_print_tree_blocks(struct btrfs_fs_info *fs_info,
				  struct cache_tree *tree, unsigned int mode)
{
	struct cache_extent *ce;
	struct extent_buffer *eb;
	struct btrfs_tree_parent_check check = { 0 };
	u64 bytenr;
	int ret = 0;

	ce = first_cache_extent(tree);
	while (ce) {
		bytenr = ce->start;

		/*
		 * Please note that here we can't check it against nodesize,
		 * as it's possible a chunk is just aligned to sectorsize but
		 * not aligned to nodesize.
		 */
		if (!IS_ALIGNED(bytenr, fs_info->sectorsize)) {
			error(
		"tree block bytenr %llu is not aligned to sectorsize %u",
			      bytenr, fs_info->sectorsize);
			ret = -EINVAL;
			goto next;
		}

		ret = check_metadata_logical(fs_info, bytenr);
		if (ret < 0)
			goto next;
		eb = read_tree_block(fs_info, bytenr, &check);
		if (!extent_buffer_uptodate(eb)) {
			error("failed to read tree block %llu", bytenr);
			ret = -EIO;
			goto next;
		}
		btrfs_print_tree(eb, mode);
		free_extent_buffer(eb);
next:
		remove_cache_extent(tree, ce);
		free(ce);
		ce = first_cache_extent(tree);
	}
	return ret;
}

static int cmd_inspect_dump_tree(const struct cmd_struct *cmd,
				 int argc, char **argv)
{
	struct btrfs_root *root;
	struct btrfs_fs_info *info;
	struct btrfs_path path = { 0 };
	struct btrfs_key key;
	struct btrfs_root_item ri;
	struct extent_buffer *leaf;
	struct btrfs_disk_key disk_key;
	struct btrfs_key found_key;
	struct cache_tree block_root;	/* for multiple --block parameters */
	struct open_ctree_args oca = { 0 };
	char uuidbuf[BTRFS_UUID_UNPARSED_SIZE];
	int ret = 0;
	int slot;
	bool extent_only = false;
	bool device_only = false;
	bool uuid_tree_only = false;
	bool roots_only = false;
	bool root_backups = false;
	int traverse = BTRFS_PRINT_TREE_DEFAULT;
	u64 block_bytenr;
	struct btrfs_root *tree_root_scan;
	u64 tree_id = 0;
	unsigned int follow = 0;
	unsigned int csum_mode = 0;
	unsigned int print_mode;

	/*
	 * For debug-tree, we care nothing about extent tree (it's just backref
	 * and usage accounting, only makes sense for RW operations).
	 * Use NO_BLOCK_GROUPS here could also speedup open_ctree() and allow us
	 * to inspect fs with corrupted extent tree blocks, and show as many good
	 * tree blocks as possible.
	 *
	 * And we want to avoid tree-checker, which can rejects the target tree
	 * block completely, while we may be debugging the problem.
	 */
	oca.flags = OPEN_CTREE_PARTIAL | OPEN_CTREE_NO_BLOCK_GROUPS |
		    OPEN_CTREE_SKIP_LEAF_ITEM_CHECKS;
	cache_tree_init(&block_root);
	optind = 0;
	while (1) {
		int c;
		enum { GETOPT_VAL_FOLLOW = GETOPT_VAL_FIRST, GETOPT_VAL_DFS,
			GETOPT_VAL_BFS,
		       GETOPT_VAL_NOSCAN, GETOPT_VAL_HIDE_NAMES,
		       GETOPT_VAL_CSUM_HEADERS, GETOPT_VAL_CSUM_ITEMS,
		};
		static const struct option long_options[] = {
			{ "extents", no_argument, NULL, 'e'},
			{ "device", no_argument, NULL, 'd'},
			{ "roots", no_argument, NULL, 'r'},
			{ "backups", no_argument, NULL, 'R'},
			{ "uuid", no_argument, NULL, 'u'},
			{ "block", required_argument, NULL, 'b'},
			{ "tree", required_argument, NULL, 't'},
			{ "follow", no_argument, NULL, GETOPT_VAL_FOLLOW },
			{ "bfs", no_argument, NULL, GETOPT_VAL_BFS },
			{ "dfs", no_argument, NULL, GETOPT_VAL_DFS },
			{ "noscan", no_argument, NULL, GETOPT_VAL_NOSCAN },
			{ "hide-names", no_argument, NULL, GETOPT_VAL_HIDE_NAMES },
			{ "csum-headers", no_argument, NULL, GETOPT_VAL_CSUM_HEADERS },
			{ "csum-items", no_argument, NULL, GETOPT_VAL_CSUM_ITEMS },
			{ NULL, 0, NULL, 0 }
		};

		c = getopt_long(argc, argv, "deb:rRut:", long_options, NULL);
		if (c < 0)
			break;
		switch (c) {
		case 'e':
			extent_only = true;
			break;
		case 'd':
			device_only = true;
			break;
		case 'r':
			roots_only = true;
			break;
		case 'u':
			uuid_tree_only = true;
			break;
		case 'R':
			roots_only = true;
			root_backups = true;
			break;
		case 'b':
			/*
			 * If only showing one block, no need to fill roots
			 * other than chunk root
			 */
			oca.flags |= __OPEN_CTREE_RETURN_CHUNK_ROOT;
			block_bytenr = arg_strtou64(optarg);
			ret = dump_add_tree_block(&block_root, block_bytenr);
			if (ret < 0)
				goto out;
			break;
		case 't':
			ret = parse_tree_id(optarg, &tree_id);
			if (ret < 0) {
				error("cannot parse tree id: %s", optarg);
				return 1;
			}
			break;
		case GETOPT_VAL_FOLLOW:
			follow = BTRFS_PRINT_TREE_FOLLOW;
			break;
		case GETOPT_VAL_DFS:
			traverse = BTRFS_PRINT_TREE_DFS;
			break;
		case GETOPT_VAL_BFS:
			traverse = BTRFS_PRINT_TREE_BFS;
			break;
		case GETOPT_VAL_NOSCAN:
			oca.flags |= OPEN_CTREE_NO_DEVICES;
			break;
		case GETOPT_VAL_HIDE_NAMES:
			oca.flags |= OPEN_CTREE_HIDE_NAMES;
			break;
		case GETOPT_VAL_CSUM_HEADERS:
			csum_mode |= BTRFS_PRINT_TREE_CSUM_HEADERS;
			break;
		case GETOPT_VAL_CSUM_ITEMS:
			csum_mode |= BTRFS_PRINT_TREE_CSUM_ITEMS;
			break;
		default:
			usage_unknown_option(cmd, argv);
		}
	}

	if (check_argc_min(argc - optind, 1))
		return 1;

	ret = btrfs_scan_argv_devices(optind, argc, argv);
	if (ret)
		return ret;

	pr_verbose(LOG_DEFAULT, "%s\n", PACKAGE_STRING);

	oca.filename = argv[optind];
	info = open_ctree_fs_info(&oca);
	if (!info) {
		error("unable to open %s", argv[optind]);
		goto out;
	}

	print_mode = follow | traverse | csum_mode;

	if (!cache_tree_empty(&block_root)) {
		root = info->chunk_root;
		ret = dump_print_tree_blocks(info, &block_root, print_mode);
		goto close_root;
	}

	root = info->fs_root;
	if (!root) {
		error("unable to open %s", argv[optind]);
		goto out;
	}

	if (!(extent_only || uuid_tree_only || tree_id)) {
		if (roots_only) {
			pr_verbose(LOG_DEFAULT, "root tree: %llu level %d\n",
			     info->tree_root->node->start,
			     btrfs_header_level(info->tree_root->node));
			pr_verbose(LOG_DEFAULT, "chunk tree: %llu level %d\n",
			     info->chunk_root->node->start,
			     btrfs_header_level(info->chunk_root->node));
			if (info->log_root_tree)
				pr_verbose(LOG_DEFAULT, "log root tree: %llu level %d\n",
				       info->log_root_tree->node->start,
					btrfs_header_level(
						info->log_root_tree->node));
		} else {
			if (info->tree_root->node) {
				pr_verbose(LOG_DEFAULT, "root tree\n");
				btrfs_print_tree(info->tree_root->node,
					BTRFS_PRINT_TREE_FOLLOW | print_mode);
			}

			if (info->chunk_root->node) {
				pr_verbose(LOG_DEFAULT, "chunk tree\n");
				btrfs_print_tree(info->chunk_root->node,
					BTRFS_PRINT_TREE_FOLLOW | print_mode);
			}

			if (info->log_root_tree) {
				pr_verbose(LOG_DEFAULT, "log root tree\n");
				btrfs_print_tree(info->log_root_tree->node,
					BTRFS_PRINT_TREE_FOLLOW | print_mode);
			}
		}
	}
	tree_root_scan = info->tree_root;

again:
	if (!extent_buffer_uptodate(tree_root_scan->node))
		goto no_node;

	/*
	 * Tree's that are not pointed by the tree of tree roots
	 */
	if (tree_id && tree_id == BTRFS_ROOT_TREE_OBJECTID) {
		if (!info->tree_root->node) {
			error("cannot print root tree, invalid pointer");
			goto close_root;
		}
		pr_verbose(LOG_DEFAULT, "root tree\n");
		btrfs_print_tree(info->tree_root->node,
					BTRFS_PRINT_TREE_FOLLOW | print_mode);
		goto close_root;
	}

	if (tree_id && tree_id == BTRFS_CHUNK_TREE_OBJECTID) {
		if (!info->chunk_root->node) {
			error("cannot print chunk tree, invalid pointer");
			goto close_root;
		}
		pr_verbose(LOG_DEFAULT, "chunk tree\n");
		btrfs_print_tree(info->chunk_root->node,
					BTRFS_PRINT_TREE_FOLLOW | print_mode);
		goto close_root;
	}

	if (tree_id && tree_id == BTRFS_TREE_LOG_OBJECTID) {
		if (!info->log_root_tree) {
			error("cannot print log root tree, invalid pointer");
			goto close_root;
		}
		pr_verbose(LOG_DEFAULT, "log root tree\n");
		btrfs_print_tree(info->log_root_tree->node,
					BTRFS_PRINT_TREE_FOLLOW | print_mode);
		goto close_root;
	}

	if (tree_id && tree_id == BTRFS_BLOCK_GROUP_TREE_OBJECTID) {
		if (!info->block_group_root) {
			error("cannot print block group tree, invalid pointer");
			goto close_root;
		}
		pr_verbose(LOG_DEFAULT, "block group tree\n");
		btrfs_print_tree(info->block_group_root->node,
					BTRFS_PRINT_TREE_FOLLOW | print_mode);
		goto close_root;
	}

	key.objectid = 0;
	key.type = BTRFS_ROOT_ITEM_KEY;
	key.offset = 0;
	ret = btrfs_search_slot(NULL, tree_root_scan, &key, &path, 0, 0);
	if (ret < 0) {
		errno = -ret;
		error("cannot read ROOT_ITEM from tree %llu: %m",
			tree_root_scan->root_key.objectid);
		goto close_root;
	}
	while (1) {
		leaf = path.nodes[0];
		slot = path.slots[0];
		if (slot >= btrfs_header_nritems(leaf)) {
			ret = btrfs_next_leaf(tree_root_scan, &path);
			if (ret != 0)
				break;
			leaf = path.nodes[0];
			slot = path.slots[0];
		}
		btrfs_item_key(leaf, &disk_key, path.slots[0]);
		btrfs_disk_key_to_cpu(&found_key, &disk_key);
		if (found_key.type == BTRFS_ROOT_ITEM_KEY) {
			unsigned long offset;
			struct extent_buffer *buf;
			struct btrfs_tree_parent_check check = {
				.owner_root = key.objectid,
			};
			bool skip = (extent_only || device_only || uuid_tree_only);

			offset = btrfs_item_ptr_offset(leaf, slot);
			read_extent_buffer(leaf, &ri, offset, sizeof(ri));
			buf = read_tree_block(info, btrfs_root_bytenr(&ri), &check);
			if (!extent_buffer_uptodate(buf))
				goto next;
			if (tree_id && found_key.objectid != tree_id) {
				free_extent_buffer(buf);
				goto next;
			}

			switch (found_key.objectid) {
			case BTRFS_ROOT_TREE_OBJECTID:
				if (!skip)
					pr_verbose(LOG_DEFAULT, "root");
				break;
			case BTRFS_EXTENT_TREE_OBJECTID:
				if (!device_only && !uuid_tree_only)
					skip = 0;
				if (!skip)
					pr_verbose(LOG_DEFAULT, "extent");
				break;
			case BTRFS_CHUNK_TREE_OBJECTID:
				if (!skip) {
					pr_verbose(LOG_DEFAULT, "chunk");
				}
				break;
			case BTRFS_DEV_TREE_OBJECTID:
				if (!uuid_tree_only)
					skip = 0;
				if (!skip)
					pr_verbose(LOG_DEFAULT, "device");
				break;
			case BTRFS_FS_TREE_OBJECTID:
				if (!skip) {
					pr_verbose(LOG_DEFAULT, "fs");
				}
				break;
			case BTRFS_ROOT_TREE_DIR_OBJECTID:
				skip = 0;
				pr_verbose(LOG_DEFAULT, "directory");
				break;
			case BTRFS_CSUM_TREE_OBJECTID:
				if (!skip) {
					pr_verbose(LOG_DEFAULT, "checksum");
				}
				break;
			case BTRFS_ORPHAN_OBJECTID:
				if (!skip) {
					pr_verbose(LOG_DEFAULT, "orphan");
				}
				break;
			case BTRFS_TREE_LOG_OBJECTID:
				if (!skip) {
					pr_verbose(LOG_DEFAULT, "log");
				}
				break;
			case BTRFS_TREE_LOG_FIXUP_OBJECTID:
				if (!skip) {
					pr_verbose(LOG_DEFAULT, "log fixup");
				}
				break;
			case BTRFS_TREE_RELOC_OBJECTID:
				if (!skip) {
					pr_verbose(LOG_DEFAULT, "reloc");
				}
				break;
			case BTRFS_DATA_RELOC_TREE_OBJECTID:
				if (!skip) {
					pr_verbose(LOG_DEFAULT, "data reloc");
				}
				break;
			case BTRFS_EXTENT_CSUM_OBJECTID:
				if (!skip) {
					pr_verbose(LOG_DEFAULT, "extent checksum");
				}
				break;
			case BTRFS_QUOTA_TREE_OBJECTID:
				if (!skip) {
					pr_verbose(LOG_DEFAULT, "quota");
				}
				break;
			case BTRFS_UUID_TREE_OBJECTID:
				if (!extent_only && !device_only)
					skip = 0;
				if (!skip)
					pr_verbose(LOG_DEFAULT, "uuid");
				break;
			case BTRFS_FREE_SPACE_TREE_OBJECTID:
				if (!skip)
					pr_verbose(LOG_DEFAULT, "free space");
				break;
			case BTRFS_MULTIPLE_OBJECTIDS:
				if (!skip) {
					pr_verbose(LOG_DEFAULT, "multiple");
				}
				break;
			case BTRFS_BLOCK_GROUP_TREE_OBJECTID:
				if (!skip)
					pr_verbose(LOG_DEFAULT, "block group");
				break;
			case BTRFS_RAID_STRIPE_TREE_OBJECTID:
				if (!skip)
					printf("raid stripe");
				break;
			default:
				if (!skip) {
					pr_verbose(LOG_DEFAULT, "file");
				}
			}
			if (extent_only && !skip) {
				pr_verbose(LOG_DEFAULT, " tree ");
				btrfs_print_key(&disk_key);
				pr_verbose(LOG_DEFAULT, "\n");
				print_extents(buf);
			} else if (!skip) {
				pr_verbose(LOG_DEFAULT, " tree ");
				btrfs_print_key(&disk_key);
				if (roots_only) {
					pr_verbose(LOG_DEFAULT, " %llu level %d\n",
					       buf->start, btrfs_header_level(buf));
				} else {
					pr_verbose(LOG_DEFAULT, " \n");
					btrfs_print_tree(buf,
						BTRFS_PRINT_TREE_FOLLOW | print_mode);
				}
			}
			free_extent_buffer(buf);
		}
next:
		path.slots[0]++;
	}
no_node:
	btrfs_release_path(&path);

	if (tree_root_scan == info->tree_root && info->log_root_tree) {
		tree_root_scan = info->log_root_tree;
		goto again;
	}

	if (extent_only || device_only || uuid_tree_only)
		goto close_root;

	if (root_backups)
		print_old_roots(info->super_copy);

	pr_verbose(LOG_DEFAULT, "total bytes %llu\n",
	       btrfs_super_total_bytes(info->super_copy));
	pr_verbose(LOG_DEFAULT, "bytes used %llu\n",
	       btrfs_super_bytes_used(info->super_copy));
	uuidbuf[BTRFS_UUID_UNPARSED_SIZE - 1] = '\0';
	uuid_unparse(info->super_copy->fsid, uuidbuf);
	pr_verbose(LOG_DEFAULT, "uuid %s\n", uuidbuf);
close_root:
	ret = close_ctree(root);
out:
	return !!ret;
}
DEFINE_SIMPLE_COMMAND(inspect_dump_tree, "dump-tree");
