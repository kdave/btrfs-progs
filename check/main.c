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
#include <fcntl.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <unistd.h>
#include <getopt.h>
#include <uuid/uuid.h>
#include "ctree.h"
#include "volumes.h"
#include "repair.h"
#include "disk-io.h"
#include "print-tree.h"
#include "task-utils.h"
#include "transaction.h"
#include "utils.h"
#include "commands.h"
#include "free-space-cache.h"
#include "free-space-tree.h"
#include "btrfsck.h"
#include "qgroup-verify.h"
#include "rbtree-utils.h"
#include "backref.h"
#include "kernel-shared/ulist.h"
#include "hash.h"
#include "help.h"
#include "check/mode-common.h"
#include "check/mode-original.h"
#include "check/mode-lowmem.h"

u64 bytes_used = 0;
u64 total_csum_bytes = 0;
u64 total_btree_bytes = 0;
u64 total_fs_tree_bytes = 0;
u64 total_extent_tree_bytes = 0;
u64 btree_space_waste = 0;
u64 data_bytes_allocated = 0;
u64 data_bytes_referenced = 0;
LIST_HEAD(duplicate_extents);
LIST_HEAD(delete_items);
int no_holes = 0;
int init_extent_tree = 0;
int check_data_csum = 0;
struct btrfs_fs_info *global_info;
struct task_ctx ctx = { 0 };
struct cache_tree *roots_info_cache = NULL;

enum btrfs_check_mode {
	CHECK_MODE_ORIGINAL,
	CHECK_MODE_LOWMEM,
	CHECK_MODE_UNKNOWN,
	CHECK_MODE_DEFAULT = CHECK_MODE_ORIGINAL
};

static enum btrfs_check_mode check_mode = CHECK_MODE_DEFAULT;

static void *print_status_check(void *p)
{
	struct task_ctx *priv = p;
	const char work_indicator[] = { '.', 'o', 'O', 'o' };
	uint32_t count = 0;
	static char *task_position_string[] = {
		"checking extents",
		"checking free space cache",
		"checking fs roots",
	};

	task_period_start(priv->info, 1000 /* 1s */);

	if (priv->tp == TASK_NOTHING)
		return NULL;

	while (1) {
		printf("%s [%c]\r", task_position_string[priv->tp],
				work_indicator[count % 4]);
		count++;
		fflush(stdout);
		task_period_wait(priv->info);
	}
	return NULL;
}

static int print_status_return(void *p)
{
	printf("\n");
	fflush(stdout);

	return 0;
}

static enum btrfs_check_mode parse_check_mode(const char *str)
{
	if (strcmp(str, "lowmem") == 0)
		return CHECK_MODE_LOWMEM;
	if (strcmp(str, "orig") == 0)
		return CHECK_MODE_ORIGINAL;
	if (strcmp(str, "original") == 0)
		return CHECK_MODE_ORIGINAL;

	return CHECK_MODE_UNKNOWN;
}

static int do_check_fs_roots(struct btrfs_fs_info *fs_info,
			  struct cache_tree *root_cache)
{
	int ret;

	if (!ctx.progress_enabled)
		fprintf(stderr, "checking fs roots\n");
	if (check_mode == CHECK_MODE_LOWMEM)
		ret = check_fs_roots_lowmem(fs_info);
	else
		ret = check_fs_roots(fs_info, root_cache);

	return ret;
}

static int do_check_chunks_and_extents(struct btrfs_fs_info *fs_info)
{
	int ret;

	if (!ctx.progress_enabled)
		fprintf(stderr, "checking extents\n");
	if (check_mode == CHECK_MODE_LOWMEM)
		ret = check_chunks_and_extents_lowmem(fs_info);
	else
		ret = check_chunks_and_extents(fs_info);

	/* Also repair device size related problems */
	if (repair && !ret) {
		ret = btrfs_fix_device_and_super_size(fs_info);
		if (ret > 0)
			ret = 0;
	}
	return ret;
}

const char * const cmd_check_usage[] = {
	"btrfs check [options] <device>",
	"Check structural integrity of a filesystem (unmounted).",
	"Check structural integrity of an unmounted filesystem. Verify internal",
	"trees' consistency and item connectivity. In the repair mode try to",
	"fix the problems found. ",
	"WARNING: the repair mode is considered dangerous",
	"",
	"-s|--super <superblock>     use this superblock copy",
	"-b|--backup                 use the first valid backup root copy",
	"--force                     skip mount checks, repair is not possible",
	"--repair                    try to repair the filesystem",
	"--readonly                  run in read-only mode (default)",
	"--init-csum-tree            create a new CRC tree",
	"--init-extent-tree          create a new extent tree",
	"--mode <MODE>               allows choice of memory/IO trade-offs",
	"                            where MODE is one of:",
	"                            original - read inodes and extents to memory (requires",
	"                                       more memory, does less IO)",
	"                            lowmem   - try to use less memory but read blocks again",
	"                                       when needed",
	"--check-data-csum           verify checksums of data blocks",
	"-Q|--qgroup-report          print a report on qgroup consistency",
	"-E|--subvol-extents <subvolid>",
	"                            print subvolume extents and sharing state",
	"-r|--tree-root <bytenr>     use the given bytenr for the tree root",
	"--chunk-root <bytenr>       use the given bytenr for the chunk tree root",
	"-p|--progress               indicate progress",
	"--clear-space-cache v1|v2   clear space cache for v1 or v2",
	NULL
};

int cmd_check(int argc, char **argv)
{
	struct cache_tree root_cache;
	struct btrfs_root *root;
	struct btrfs_fs_info *info;
	u64 bytenr = 0;
	u64 subvolid = 0;
	u64 tree_root_bytenr = 0;
	u64 chunk_root_bytenr = 0;
	char uuidbuf[BTRFS_UUID_UNPARSED_SIZE];
	int ret = 0;
	int err = 0;
	u64 num;
	int init_csum_tree = 0;
	int readonly = 0;
	int clear_space_cache = 0;
	int qgroup_report = 0;
	int qgroups_repaired = 0;
	unsigned ctree_flags = OPEN_CTREE_EXCLUSIVE;
	int force = 0;

	while(1) {
		int c;
		enum { GETOPT_VAL_REPAIR = 257, GETOPT_VAL_INIT_CSUM,
			GETOPT_VAL_INIT_EXTENT, GETOPT_VAL_CHECK_CSUM,
			GETOPT_VAL_READONLY, GETOPT_VAL_CHUNK_TREE,
			GETOPT_VAL_MODE, GETOPT_VAL_CLEAR_SPACE_CACHE,
			GETOPT_VAL_FORCE };
		static const struct option long_options[] = {
			{ "super", required_argument, NULL, 's' },
			{ "repair", no_argument, NULL, GETOPT_VAL_REPAIR },
			{ "readonly", no_argument, NULL, GETOPT_VAL_READONLY },
			{ "init-csum-tree", no_argument, NULL,
				GETOPT_VAL_INIT_CSUM },
			{ "init-extent-tree", no_argument, NULL,
				GETOPT_VAL_INIT_EXTENT },
			{ "check-data-csum", no_argument, NULL,
				GETOPT_VAL_CHECK_CSUM },
			{ "backup", no_argument, NULL, 'b' },
			{ "subvol-extents", required_argument, NULL, 'E' },
			{ "qgroup-report", no_argument, NULL, 'Q' },
			{ "tree-root", required_argument, NULL, 'r' },
			{ "chunk-root", required_argument, NULL,
				GETOPT_VAL_CHUNK_TREE },
			{ "progress", no_argument, NULL, 'p' },
			{ "mode", required_argument, NULL,
				GETOPT_VAL_MODE },
			{ "clear-space-cache", required_argument, NULL,
				GETOPT_VAL_CLEAR_SPACE_CACHE},
			{ "force", no_argument, NULL, GETOPT_VAL_FORCE },
			{ NULL, 0, NULL, 0}
		};

		c = getopt_long(argc, argv, "as:br:pEQ", long_options, NULL);
		if (c < 0)
			break;
		switch(c) {
			case 'a': /* ignored */ break;
			case 'b':
				ctree_flags |= OPEN_CTREE_BACKUP_ROOT;
				break;
			case 's':
				num = arg_strtou64(optarg);
				if (num >= BTRFS_SUPER_MIRROR_MAX) {
					error(
					"super mirror should be less than %d",
						BTRFS_SUPER_MIRROR_MAX);
					exit(1);
				}
				bytenr = btrfs_sb_offset(((int)num));
				printf("using SB copy %llu, bytenr %llu\n", num,
				       (unsigned long long)bytenr);
				break;
			case 'Q':
				qgroup_report = 1;
				break;
			case 'E':
				subvolid = arg_strtou64(optarg);
				break;
			case 'r':
				tree_root_bytenr = arg_strtou64(optarg);
				break;
			case GETOPT_VAL_CHUNK_TREE:
				chunk_root_bytenr = arg_strtou64(optarg);
				break;
			case 'p':
				ctx.progress_enabled = true;
				break;
			case '?':
			case 'h':
				usage(cmd_check_usage);
			case GETOPT_VAL_REPAIR:
				printf("enabling repair mode\n");
				repair = 1;
				ctree_flags |= OPEN_CTREE_WRITES;
				break;
			case GETOPT_VAL_READONLY:
				readonly = 1;
				break;
			case GETOPT_VAL_INIT_CSUM:
				printf("Creating a new CRC tree\n");
				init_csum_tree = 1;
				repair = 1;
				ctree_flags |= OPEN_CTREE_WRITES;
				break;
			case GETOPT_VAL_INIT_EXTENT:
				init_extent_tree = 1;
				ctree_flags |= (OPEN_CTREE_WRITES |
						OPEN_CTREE_NO_BLOCK_GROUPS);
				repair = 1;
				break;
			case GETOPT_VAL_CHECK_CSUM:
				check_data_csum = 1;
				break;
			case GETOPT_VAL_MODE:
				check_mode = parse_check_mode(optarg);
				if (check_mode == CHECK_MODE_UNKNOWN) {
					error("unknown mode: %s", optarg);
					exit(1);
				}
				break;
			case GETOPT_VAL_CLEAR_SPACE_CACHE:
				if (strcmp(optarg, "v1") == 0) {
					clear_space_cache = 1;
				} else if (strcmp(optarg, "v2") == 0) {
					clear_space_cache = 2;
					ctree_flags |= OPEN_CTREE_INVALIDATE_FST;
				} else {
					error(
		"invalid argument to --clear-space-cache, must be v1 or v2");
					exit(1);
				}
				ctree_flags |= OPEN_CTREE_WRITES;
				break;
			case GETOPT_VAL_FORCE:
				force = 1;
				break;
		}
	}

	if (check_argc_exact(argc - optind, 1))
		usage(cmd_check_usage);

	if (ctx.progress_enabled) {
		ctx.tp = TASK_NOTHING;
		ctx.info = task_init(print_status_check, print_status_return, &ctx);
	}

	/* This check is the only reason for --readonly to exist */
	if (readonly && repair) {
		error("repair options are not compatible with --readonly");
		exit(1);
	}

	/*
	 * experimental and dangerous
	 */
	if (repair && check_mode == CHECK_MODE_LOWMEM)
		warning("low-memory mode repair support is only partial");

	radix_tree_init();
	cache_tree_init(&root_cache);

	ret = check_mounted(argv[optind]);
	if (!force) {
		if (ret < 0) {
			error("could not check mount status: %s",
					strerror(-ret));
			err |= !!ret;
			goto err_out;
		} else if (ret) {
			error(
"%s is currently mounted, use --force if you really intend to check the filesystem",
				argv[optind]);
			ret = -EBUSY;
			err |= !!ret;
			goto err_out;
		}
	} else {
		if (repair) {
			error("repair and --force is not yet supported");
			ret = 1;
			err |= !!ret;
			goto err_out;
		}
		if (ret < 0) {
			warning(
"cannot check mount status of %s, the filesystem could be mounted, continuing because of --force",
				argv[optind]);
		} else if (ret) {
			warning(
			"filesystem mounted, continuing because of --force");
		}
		/* A block device is mounted in exclusive mode by kernel */
		ctree_flags &= ~OPEN_CTREE_EXCLUSIVE;
	}

	/* only allow partial opening under repair mode */
	if (repair)
		ctree_flags |= OPEN_CTREE_PARTIAL;

	info = open_ctree_fs_info(argv[optind], bytenr, tree_root_bytenr,
				  chunk_root_bytenr, ctree_flags);
	if (!info) {
		error("cannot open file system");
		ret = -EIO;
		err |= !!ret;
		goto err_out;
	}

	global_info = info;
	root = info->fs_root;
	uuid_unparse(info->super_copy->fsid, uuidbuf);

	printf("Checking filesystem on %s\nUUID: %s\n", argv[optind], uuidbuf);

	/*
	 * Check the bare minimum before starting anything else that could rely
	 * on it, namely the tree roots, any local consistency checks
	 */
	if (!extent_buffer_uptodate(info->tree_root->node) ||
	    !extent_buffer_uptodate(info->dev_root->node) ||
	    !extent_buffer_uptodate(info->chunk_root->node)) {
		error("critical roots corrupted, unable to check the filesystem");
		err |= !!ret;
		ret = -EIO;
		goto close_out;
	}

	if (clear_space_cache) {
		ret = do_clear_free_space_cache(info, clear_space_cache);
		err |= !!ret;
		goto close_out;
	}

	/*
	 * repair mode will force us to commit transaction which
	 * will make us fail to load log tree when mounting.
	 */
	if (repair && btrfs_super_log_root(info->super_copy)) {
		ret = ask_user("repair mode will force to clear out log tree, are you sure?");
		if (!ret) {
			ret = 1;
			err |= !!ret;
			goto close_out;
		}
		ret = zero_log_tree(root);
		err |= !!ret;
		if (ret) {
			error("failed to zero log tree: %d", ret);
			goto close_out;
		}
	}

	if (qgroup_report) {
		printf("Print quota groups for %s\nUUID: %s\n", argv[optind],
		       uuidbuf);
		ret = qgroup_verify_all(info);
		err |= !!ret;
		if (ret == 0)
			report_qgroups(1);
		goto close_out;
	}
	if (subvolid) {
		printf("Print extent state for subvolume %llu on %s\nUUID: %s\n",
		       subvolid, argv[optind], uuidbuf);
		ret = print_extent_state(info, subvolid);
		err |= !!ret;
		goto close_out;
	}

	if (init_extent_tree || init_csum_tree) {
		struct btrfs_trans_handle *trans;

		trans = btrfs_start_transaction(info->extent_root, 0);
		if (IS_ERR(trans)) {
			error("error starting transaction");
			ret = PTR_ERR(trans);
			err |= !!ret;
			goto close_out;
		}

		if (init_extent_tree) {
			printf("Creating a new extent tree\n");
			ret = reinit_extent_tree(trans, info);
			err |= !!ret;
			if (ret)
				goto close_out;
		}

		if (init_csum_tree) {
			printf("Reinitialize checksum tree\n");
			ret = btrfs_fsck_reinit_root(trans, info->csum_root, 0);
			if (ret) {
				error("checksum tree initialization failed: %d",
						ret);
				ret = -EIO;
				err |= !!ret;
				goto close_out;
			}

			ret = fill_csum_tree(trans, info->csum_root,
					     init_extent_tree);
			err |= !!ret;
			if (ret) {
				error("checksum tree refilling failed: %d", ret);
				return -EIO;
			}
		}
		/*
		 * Ok now we commit and run the normal fsck, which will add
		 * extent entries for all of the items it finds.
		 */
		ret = btrfs_commit_transaction(trans, info->extent_root);
		err |= !!ret;
		if (ret)
			goto close_out;
	}
	if (!extent_buffer_uptodate(info->extent_root->node)) {
		error("critical: extent_root, unable to check the filesystem");
		ret = -EIO;
		err |= !!ret;
		goto close_out;
	}
	if (!extent_buffer_uptodate(info->csum_root->node)) {
		error("critical: csum_root, unable to check the filesystem");
		ret = -EIO;
		err |= !!ret;
		goto close_out;
	}

	if (!init_extent_tree) {
		ret = repair_root_items(info);
		if (ret < 0) {
			err = !!ret;
			error("failed to repair root items: %s", strerror(-ret));
			goto close_out;
		}
		if (repair) {
			fprintf(stderr, "Fixed %d roots.\n", ret);
			ret = 0;
		} else if (ret > 0) {
			fprintf(stderr,
				"Found %d roots with an outdated root item.\n",
				ret);
			fprintf(stderr,
	"Please run a filesystem check with the option --repair to fix them.\n");
			ret = 1;
			err |= ret;
			goto close_out;
		}
	}

	ret = do_check_chunks_and_extents(info);
	err |= !!ret;
	if (ret)
		error(
		"errors found in extent allocation tree or chunk allocation");

	/* Only re-check super size after we checked and repaired the fs */
	err |= !is_super_size_valid(info);

	if (!ctx.progress_enabled) {
		if (btrfs_fs_compat_ro(info, FREE_SPACE_TREE))
			fprintf(stderr, "checking free space tree\n");
		else
			fprintf(stderr, "checking free space cache\n");
	}
	ret = check_space_cache(root);
	err |= !!ret;
	if (ret) {
		if (btrfs_fs_compat_ro(info, FREE_SPACE_TREE))
			error("errors found in free space tree");
		else
			error("errors found in free space cache");
		goto out;
	}

	/*
	 * We used to have to have these hole extents in between our real
	 * extents so if we don't have this flag set we need to make sure there
	 * are no gaps in the file extents for inodes, otherwise we can just
	 * ignore it when this happens.
	 */
	no_holes = btrfs_fs_incompat(root->fs_info, NO_HOLES);
	ret = do_check_fs_roots(info, &root_cache);
	err |= !!ret;
	if (ret) {
		error("errors found in fs roots");
		goto out;
	}

	fprintf(stderr, "checking csums\n");
	ret = check_csums(root);
	err |= !!ret;
	if (ret) {
		error("errors found in csum tree");
		goto out;
	}

	fprintf(stderr, "checking root refs\n");
	/* For low memory mode, check_fs_roots_v2 handles root refs */
	if (check_mode != CHECK_MODE_LOWMEM) {
		ret = check_root_refs(root, &root_cache);
		err |= !!ret;
		if (ret) {
			error("errors found in root refs");
			goto out;
		}
	}

	while (repair && !list_empty(&root->fs_info->recow_ebs)) {
		struct extent_buffer *eb;

		eb = list_first_entry(&root->fs_info->recow_ebs,
				      struct extent_buffer, recow);
		list_del_init(&eb->recow);
		ret = recow_extent_buffer(root, eb);
		err |= !!ret;
		if (ret) {
			error("fails to fix transid errors");
			break;
		}
	}

	while (!list_empty(&delete_items)) {
		struct bad_item *bad;

		bad = list_first_entry(&delete_items, struct bad_item, list);
		list_del_init(&bad->list);
		if (repair) {
			ret = delete_bad_item(root, bad);
			err |= !!ret;
		}
		free(bad);
	}

	if (info->quota_enabled) {
		fprintf(stderr, "checking quota groups\n");
		ret = qgroup_verify_all(info);
		err |= !!ret;
		if (ret) {
			error("failed to check quota groups");
			goto out;
		}
		report_qgroups(0);
		ret = repair_qgroups(info, &qgroups_repaired);
		err |= !!ret;
		if (err) {
			error("failed to repair quota groups");
			goto out;
		}
		ret = 0;
	}

	if (!list_empty(&root->fs_info->recow_ebs)) {
		error("transid errors in file system");
		ret = 1;
		err |= !!ret;
	}
out:
	printf("found %llu bytes used, ",
	       (unsigned long long)bytes_used);
	if (err)
		printf("error(s) found\n");
	else
		printf("no error found\n");
	printf("total csum bytes: %llu\n",(unsigned long long)total_csum_bytes);
	printf("total tree bytes: %llu\n",
	       (unsigned long long)total_btree_bytes);
	printf("total fs tree bytes: %llu\n",
	       (unsigned long long)total_fs_tree_bytes);
	printf("total extent tree bytes: %llu\n",
	       (unsigned long long)total_extent_tree_bytes);
	printf("btree space waste bytes: %llu\n",
	       (unsigned long long)btree_space_waste);
	printf("file data blocks allocated: %llu\n referenced %llu\n",
		(unsigned long long)data_bytes_allocated,
		(unsigned long long)data_bytes_referenced);

	free_qgroup_counts();
	free_root_recs_tree(&root_cache);
close_out:
	close_ctree(root);
err_out:
	if (ctx.progress_enabled)
		task_deinit(ctx.info);

	return err;
}
