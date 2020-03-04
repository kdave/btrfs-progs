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

#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <uuid/uuid.h>
#include <getopt.h>
#include <fcntl.h>

#include "kerncompat.h"
#include "kernel-lib/radix-tree.h"
#include "ctree.h"
#include "disk-io.h"
#include "print-tree.h"
#include "transaction.h"
#include "volumes.h"
#include "cmds/commands.h"
#include "common/utils.h"
#include "common/help.h"
#include "common/device-scan.h"

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
		next = read_tree_block(fs_info,
				btrfs_node_blockptr(eb, i),
				btrfs_node_ptr_generation(eb, i));
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
	struct btrfs_root_backup *backup;
	int i;

	for (i = 0; i < BTRFS_NUM_BACKUP_ROOTS; i++) {
		backup = super->super_roots + i;
		printf("btrfs root backup slot %d\n", i);
		printf("\ttree root gen %llu block %llu\n",
		       (unsigned long long)btrfs_backup_tree_root_gen(backup),
		       (unsigned long long)btrfs_backup_tree_root(backup));

		printf("\t\textent root gen %llu block %llu\n",
		       (unsigned long long)btrfs_backup_extent_root_gen(backup),
		       (unsigned long long)btrfs_backup_extent_root(backup));

		printf("\t\tchunk root gen %llu block %llu\n",
		       (unsigned long long)btrfs_backup_chunk_root_gen(backup),
		       (unsigned long long)btrfs_backup_chunk_root(backup));

		printf("\t\tdevice root gen %llu block %llu\n",
		       (unsigned long long)btrfs_backup_dev_root_gen(backup),
		       (unsigned long long)btrfs_backup_dev_root(backup));

		printf("\t\tcsum root gen %llu block %llu\n",
		       (unsigned long long)btrfs_backup_csum_root_gen(backup),
		       (unsigned long long)btrfs_backup_csum_root(backup));

		printf("\t\tfs root gen %llu block %llu\n",
		       (unsigned long long)btrfs_backup_fs_root_gen(backup),
		       (unsigned long long)btrfs_backup_fs_root(backup));

		printf("\t\t%llu used %llu total %llu devices\n",
		       (unsigned long long)btrfs_backup_bytes_used(backup),
		       (unsigned long long)btrfs_backup_total_bytes(backup),
		       (unsigned long long)btrfs_backup_num_devices(backup));
	}
}

/*
 * Convert a tree name from various forms to the numerical id if possible
 * Accepted forms:
 * - case does not matter
 * - same as the key name, BTRFS_ROOT_TREE_OBJECTID
 * - dtto shortened, BTRFS_ROOT_TREE
 * - dtto without prefix, ROOT_TREE
 * - common name, ROOT, CHUNK, EXTENT, ...
 * - dtto alias, DEVICE for DEV, CHECKSUM for CSUM
 *
 * Returns 0 if the tree id was not recognized.
 */
static u64 treeid_from_string(const char *str, const char **end)
{
	int match = 0;
	int i;
	u64 id;
	static struct treename {
		const char *name;
		u64 id;
	} tn[] = {
		{ "ROOT", BTRFS_ROOT_TREE_OBJECTID },
		{ "EXTENT", BTRFS_EXTENT_TREE_OBJECTID },
		{ "CHUNK", BTRFS_CHUNK_TREE_OBJECTID },
		{ "DEVICE", BTRFS_DEV_TREE_OBJECTID },
		{ "DEV", BTRFS_DEV_TREE_OBJECTID },
		{ "FS", BTRFS_FS_TREE_OBJECTID },
		{ "CSUM", BTRFS_CSUM_TREE_OBJECTID },
		{ "CHECKSUM", BTRFS_CSUM_TREE_OBJECTID },
		{ "QUOTA", BTRFS_QUOTA_TREE_OBJECTID },
		{ "UUID", BTRFS_UUID_TREE_OBJECTID },
		{ "FREE_SPACE", BTRFS_FREE_SPACE_TREE_OBJECTID },
		{ "TREE_LOG_FIXUP", BTRFS_TREE_LOG_FIXUP_OBJECTID },
		{ "TREE_LOG", BTRFS_TREE_LOG_OBJECTID },
		{ "TREE_RELOC", BTRFS_TREE_RELOC_OBJECTID },
		{ "DATA_RELOC", BTRFS_DATA_RELOC_TREE_OBJECTID }
	};

	if (strncasecmp("BTRFS_", str, strlen("BTRFS_")) == 0)
		str += strlen("BTRFS_");

	for (i = 0; i < ARRAY_SIZE(tn); i++) {
		int len = strlen(tn[i].name);

		if (strncasecmp(tn[i].name, str, len) == 0) {
			id = tn[i].id;
			match = 1;
			str += len;
			break;
		}
	}

	if (!match)
		return 0;

	if (strncasecmp("_TREE", str, strlen("_TREE")) == 0)
		str += strlen("_TREE");

	if (strncasecmp("_OBJECTID", str, strlen("_OBJECTID")) == 0)
		str += strlen("_OBJECTID");

	*end = str;

	return id;
}

static const char * const cmd_inspect_dump_tree_usage[] = {
	"btrfs inspect-internal dump-tree [options] <device> [<device> ..]",
	"Dump tree structures from a given device",
	"Dump tree structures from a given device in textual form, expand keys to human",
	"readable equivalents where possible.",
	"Note: contains file names, consider that if you're asked to send the dump",
	"for analysis.",
	"",
	"-e|--extents           print only extent info: extent and device trees",
	"-d|--device            print only device info: tree root, chunk and device trees",
	"-r|--roots             print only short root node info",
	"-R|--backups           same as --roots plus print backup root info",
	"-u|--uuid              print only the uuid tree",
	"-b|--block <block_num> print info from the specified block only",
	"                       can be specified multiple times",
	"-t|--tree <tree_id>    print only tree with the given id (string or number)",
	"--follow               use with -b, to show all children tree blocks of <block_num>",
	"--noscan               do not scan the devices from the filesystem, use only the listed ones",
	"--bfs                  breadth-first traversal of the trees, print nodes, then leaves (default)",
	"--dfs                  depth-first traversal of the trees",
	"--hide-names           hide filenames/subvolume/xattrs and other name references",
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
		error("failed to record tree block bytenr %llu: %d(%s)",
			bytenr, ret, strerror(-ret));
		return ret;
	}
	return ret;
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
				  struct cache_tree *tree, bool follow)
{
	struct cache_extent *ce;
	struct extent_buffer *eb;
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

		eb = read_tree_block(fs_info, bytenr, 0);
		if (!extent_buffer_uptodate(eb)) {
			error("failed to read tree block %llu", bytenr);
			ret = -EIO;
			goto next;
		}
		btrfs_print_tree(eb, follow, BTRFS_PRINT_TREE_DEFAULT);
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
	struct btrfs_path path;
	struct btrfs_key key;
	struct btrfs_root_item ri;
	struct extent_buffer *leaf;
	struct btrfs_disk_key disk_key;
	struct btrfs_key found_key;
	struct cache_tree block_root;	/* for multiple --block parameters */
	char uuidbuf[BTRFS_UUID_UNPARSED_SIZE];
	int ret = 0;
	int slot;
	int extent_only = 0;
	int device_only = 0;
	int uuid_tree_only = 0;
	int roots_only = 0;
	int root_backups = 0;
	int traverse = BTRFS_PRINT_TREE_DEFAULT;
	int dev_optind;
	unsigned open_ctree_flags;
	u64 block_bytenr;
	struct btrfs_root *tree_root_scan;
	u64 tree_id = 0;
	bool follow = false;

	/*
	 * For debug-tree, we care nothing about extent tree (it's just backref
	 * and usage accounting, only makes sense for RW operations).
	 * Use NO_BLOCK_GROUPS here could also speedup open_ctree() and allow us
	 * to inspect fs with corrupted extent tree blocks, and show as many good
	 * tree blocks as possible.
	 */
	open_ctree_flags = OPEN_CTREE_PARTIAL | OPEN_CTREE_NO_BLOCK_GROUPS;
	cache_tree_init(&block_root);
	optind = 0;
	while (1) {
		int c;
		enum { GETOPT_VAL_FOLLOW = 256, GETOPT_VAL_DFS, GETOPT_VAL_BFS,
		       GETOPT_VAL_NOSCAN, GETOPT_VAL_HIDE_NAMES };
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
			{ NULL, 0, NULL, 0 }
		};

		c = getopt_long(argc, argv, "deb:rRut:", long_options, NULL);
		if (c < 0)
			break;
		switch (c) {
		case 'e':
			extent_only = 1;
			break;
		case 'd':
			device_only = 1;
			break;
		case 'r':
			roots_only = 1;
			break;
		case 'u':
			uuid_tree_only = 1;
			break;
		case 'R':
			roots_only = 1;
			root_backups = 1;
			break;
		case 'b':
			/*
			 * If only showing one block, no need to fill roots
			 * other than chunk root
			 */
			open_ctree_flags |= __OPEN_CTREE_RETURN_CHUNK_ROOT;
			block_bytenr = arg_strtou64(optarg);
			ret = dump_add_tree_block(&block_root, block_bytenr);
			if (ret < 0)
				goto out;
			break;
		case 't': {
			const char *end = NULL;

			if (string_is_numerical(optarg))
				tree_id = arg_strtou64(optarg);
			else
				tree_id = treeid_from_string(optarg, &end);

			if (!tree_id) {
				error("unrecognized tree id: %s",
						optarg);
				exit(1);
			}

			if (end && *end) {
				error("unexpected tree id suffix of '%s': %s",
						optarg, end);
				exit(1);
			}
			break;
			}
		case GETOPT_VAL_FOLLOW:
			follow = true;
			break;
		case GETOPT_VAL_DFS:
			traverse = BTRFS_PRINT_TREE_DFS;
			break;
		case GETOPT_VAL_BFS:
			traverse = BTRFS_PRINT_TREE_BFS;
			break;
		case GETOPT_VAL_NOSCAN:
			open_ctree_flags |= OPEN_CTREE_NO_DEVICES;
			break;
		case GETOPT_VAL_HIDE_NAMES:
			open_ctree_flags |= OPEN_CTREE_HIDE_NAMES;
			break;
		default:
			usage_unknown_option(cmd, argv);
		}
	}

	if (check_argc_min(argc - optind, 1))
		return 1;

	dev_optind = optind;
	while (dev_optind < argc) {
		int fd;
		struct btrfs_fs_devices *fs_devices;
		u64 num_devices;

		ret = check_arg_type(argv[optind]);
		if (ret != BTRFS_ARG_BLKDEV && ret != BTRFS_ARG_REG) {
			if (ret < 0) {
				errno = -ret;
				error("invalid argument %s: %m", argv[dev_optind]);
			} else {
				error("not a block device or regular file: %s",
				       argv[dev_optind]);
			}
		}
		fd = open(argv[dev_optind], O_RDONLY);
		if (fd < 0) {
			error("cannot open %s: %m", argv[dev_optind]);
			return -EINVAL;
		}
		ret = btrfs_scan_one_device(fd, argv[dev_optind], &fs_devices,
					    &num_devices,
					    BTRFS_SUPER_INFO_OFFSET,
					    SBREAD_DEFAULT);
		close(fd);
		if (ret < 0) {
			errno = -ret;
			error("device scan %s: %m", argv[dev_optind]);
			return ret;
		}
		dev_optind++;
	}

	printf("%s\n", PACKAGE_STRING);

	info = open_ctree_fs_info(argv[optind], 0, 0, 0, open_ctree_flags);
	if (!info) {
		error("unable to open %s", argv[optind]);
		goto out;
	}

	if (!cache_tree_empty(&block_root)) {
		root = info->chunk_root;
		ret = dump_print_tree_blocks(info, &block_root, follow);
		goto close_root;
	}

	root = info->fs_root;
	if (!root) {
		error("unable to open %s", argv[optind]);
		goto out;
	}

	if (!(extent_only || uuid_tree_only || tree_id)) {
		if (roots_only) {
			printf("root tree: %llu level %d\n",
			     (unsigned long long)info->tree_root->node->start,
			     btrfs_header_level(info->tree_root->node));
			printf("chunk tree: %llu level %d\n",
			     (unsigned long long)info->chunk_root->node->start,
			     btrfs_header_level(info->chunk_root->node));
			if (info->log_root_tree)
				printf("log root tree: %llu level %d\n",
				       info->log_root_tree->node->start,
					btrfs_header_level(
						info->log_root_tree->node));
		} else {
			if (info->tree_root->node) {
				printf("root tree\n");
				btrfs_print_tree(info->tree_root->node, true,
						 traverse);
			}

			if (info->chunk_root->node) {
				printf("chunk tree\n");
				btrfs_print_tree(info->chunk_root->node, true,
						 traverse);
			}

			if (info->log_root_tree) {
				printf("log root tree\n");
				btrfs_print_tree(info->log_root_tree->node,
						 true, traverse);
			}
		}
	}
	tree_root_scan = info->tree_root;

	btrfs_init_path(&path);
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
		printf("root tree\n");
		btrfs_print_tree(info->tree_root->node, true, traverse);
		goto close_root;
	}

	if (tree_id && tree_id == BTRFS_CHUNK_TREE_OBJECTID) {
		if (!info->chunk_root->node) {
			error("cannot print chunk tree, invalid pointer");
			goto close_root;
		}
		printf("chunk tree\n");
		btrfs_print_tree(info->chunk_root->node, true, traverse);
		goto close_root;
	}

	if (tree_id && tree_id == BTRFS_TREE_LOG_OBJECTID) {
		if (!info->log_root_tree) {
			error("cannot print log root tree, invalid pointer");
			goto close_root;
		}
		printf("log root tree\n");
		btrfs_print_tree(info->log_root_tree->node, true, traverse);
		goto close_root;
	}

	key.offset = 0;
	key.objectid = 0;
	key.type = BTRFS_ROOT_ITEM_KEY;
	ret = btrfs_search_slot(NULL, tree_root_scan, &key, &path, 0, 0);
	if (ret < 0) {
		errno = -ret;
		error("cannot read ROOT_ITEM from tree %llu: %m",
			(unsigned long long)tree_root_scan->root_key.objectid);
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
			int skip = extent_only | device_only | uuid_tree_only;

			offset = btrfs_item_ptr_offset(leaf, slot);
			read_extent_buffer(leaf, &ri, offset, sizeof(ri));
			buf = read_tree_block(info, btrfs_root_bytenr(&ri), 0);
			if (!extent_buffer_uptodate(buf))
				goto next;
			if (tree_id && found_key.objectid != tree_id) {
				free_extent_buffer(buf);
				goto next;
			}

			switch (found_key.objectid) {
			case BTRFS_ROOT_TREE_OBJECTID:
				if (!skip)
					printf("root");
				break;
			case BTRFS_EXTENT_TREE_OBJECTID:
				if (!device_only && !uuid_tree_only)
					skip = 0;
				if (!skip)
					printf("extent");
				break;
			case BTRFS_CHUNK_TREE_OBJECTID:
				if (!skip) {
					printf("chunk");
				}
				break;
			case BTRFS_DEV_TREE_OBJECTID:
				if (!uuid_tree_only)
					skip = 0;
				if (!skip)
					printf("device");
				break;
			case BTRFS_FS_TREE_OBJECTID:
				if (!skip) {
					printf("fs");
				}
				break;
			case BTRFS_ROOT_TREE_DIR_OBJECTID:
				skip = 0;
				printf("directory");
				break;
			case BTRFS_CSUM_TREE_OBJECTID:
				if (!skip) {
					printf("checksum");
				}
				break;
			case BTRFS_ORPHAN_OBJECTID:
				if (!skip) {
					printf("orphan");
				}
				break;
			case BTRFS_TREE_LOG_OBJECTID:
				if (!skip) {
					printf("log");
				}
				break;
			case BTRFS_TREE_LOG_FIXUP_OBJECTID:
				if (!skip) {
					printf("log fixup");
				}
				break;
			case BTRFS_TREE_RELOC_OBJECTID:
				if (!skip) {
					printf("reloc");
				}
				break;
			case BTRFS_DATA_RELOC_TREE_OBJECTID:
				if (!skip) {
					printf("data reloc");
				}
				break;
			case BTRFS_EXTENT_CSUM_OBJECTID:
				if (!skip) {
					printf("extent checksum");
				}
				break;
			case BTRFS_QUOTA_TREE_OBJECTID:
				if (!skip) {
					printf("quota");
				}
				break;
			case BTRFS_UUID_TREE_OBJECTID:
				if (!extent_only && !device_only)
					skip = 0;
				if (!skip)
					printf("uuid");
				break;
			case BTRFS_FREE_SPACE_TREE_OBJECTID:
				if (!skip)
					printf("free space");
				break;
			case BTRFS_MULTIPLE_OBJECTIDS:
				if (!skip) {
					printf("multiple");
				}
				break;
			default:
				if (!skip) {
					printf("file");
				}
			}
			if (extent_only && !skip) {
				printf(" tree ");
				btrfs_print_key(&disk_key);
				printf("\n");
				print_extents(buf);
			} else if (!skip) {
				printf(" tree ");
				btrfs_print_key(&disk_key);
				if (roots_only) {
					printf(" %llu level %d\n",
					       (unsigned long long)buf->start,
					       btrfs_header_level(buf));
				} else {
					printf(" \n");
					btrfs_print_tree(buf, true, traverse);
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

	printf("total bytes %llu\n",
	       (unsigned long long)btrfs_super_total_bytes(info->super_copy));
	printf("bytes used %llu\n",
	       (unsigned long long)btrfs_super_bytes_used(info->super_copy));
	uuidbuf[BTRFS_UUID_UNPARSED_SIZE - 1] = '\0';
	uuid_unparse(info->super_copy->fsid, uuidbuf);
	printf("uuid %s\n", uuidbuf);
close_root:
	ret = close_ctree(root);
out:
	return !!ret;
}
DEFINE_SIMPLE_COMMAND(inspect_dump_tree, "dump-tree");
