/*
 * Copyright (C) 2008 Oracle.  All rights reserved.
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
#include <stdio.h>
#include <fcntl.h>
#include <unistd.h>
#include <getopt.h>
#include <errno.h>
#include <stdbool.h>
#include <uuid/uuid.h>
#include "kernel-shared/ctree.h"
#include "kernel-shared/disk-io.h"
#include "kernel-shared/transaction.h"
#include "kernel-shared/volumes.h"
#include "common/utils.h"
#include "common/open-utils.h"
#include "common/parse-utils.h"
#include "common/device-scan.h"
#include "common/messages.h"
#include "common/string-utils.h"
#include "common/help.h"
#include "common/box.h"
#include "cmds/commands.h"
#include "tune/tune.h"

static char *device;
static int force = 0;

static int set_super_incompat_flags(struct btrfs_root *root, u64 flags)
{
	struct btrfs_trans_handle *trans;
	struct btrfs_super_block *disk_super;
	u64 super_flags;
	int ret;

	disk_super = root->fs_info->super_copy;
	super_flags = btrfs_super_incompat_flags(disk_super);
	super_flags |= flags;
	trans = btrfs_start_transaction(root, 1);
	BUG_ON(IS_ERR(trans));
	btrfs_set_super_incompat_flags(disk_super, super_flags);
	ret = btrfs_commit_transaction(trans, root);

	return ret;
}

static const char * const tune_usage[] = {
	"btrfstune [options] device",
	"Tune settings of filesystem features on an unmounted device",
	"",
	"Options:",
	"Change feature status:",
	OPTLINE("-r", "enable extended inode refs (mkfs: extref, for hardlink limits)"),
	OPTLINE("-x", "enable skinny metadata extent refs (mkfs: skinny-metadata)"),
	OPTLINE("-n", "enable no-holes feature (mkfs: no-holes, more efficient sparse file representation)"),
	OPTLINE("-S <0|1>", "set/unset seeding status of a device"),
	"",
	"UUID changes:",
	OPTLINE("-u", "rewrite fsid, use a random one"),
	OPTLINE("-U UUID", "rewrite fsid to UUID"),
	OPTLINE("-m", "change fsid in metadata_uuid to a random UUID incompat change, more lightweight than -u|-U)"),
	OPTLINE("-M UUID", "change fsid in metadata_uuid to UUID"),
	"",
	"General:",
	OPTLINE("-f", "allow dangerous operations, make sure that you are aware of the dangers"),
	OPTLINE("--help", "print this help"),
#if EXPERIMENTAL
	"",
	"EXPERIMENTAL FEATURES:",
	OPTLINE("--csum CSUM", "switch checksum for data and metadata to CSUM"),
	OPTLINE("-b", "enable block group tree (mkfs: block-group-tree, for less mount time)"),
#endif
	NULL
};

static const struct cmd_struct tune_cmd = {
	.usagestr = tune_usage
};

int BOX_MAIN(btrfstune)(int argc, char *argv[])
{
	struct btrfs_root *root;
	unsigned ctree_flags = OPEN_CTREE_WRITES;
	int success = 0;
	int total = 0;
	int seeding_flag = 0;
	u64 seeding_value = 0;
	int random_fsid = 0;
	int change_metadata_uuid = 0;
	bool to_bg_tree = false;
	int csum_type = -1;
	char *new_fsid_str = NULL;
	int ret;
	u64 super_flags = 0;
	int fd = -1;

	btrfs_config_init();

	while(1) {
		enum { GETOPT_VAL_CSUM = GETOPT_VAL_FIRST };
		static const struct option long_options[] = {
			{ "help", no_argument, NULL, GETOPT_VAL_HELP},
#if EXPERIMENTAL
			{ "csum", required_argument, NULL, GETOPT_VAL_CSUM },
#endif
			{ NULL, 0, NULL, 0 }
		};
#if EXPERIMENTAL
		int c = getopt_long(argc, argv, "S:rxfuU:nmM:b", long_options, NULL);
#else
		int c = getopt_long(argc, argv, "S:rxfuU:nmM:", long_options, NULL);
#endif

		if (c < 0)
			break;
		switch(c) {
		case 'b':
			btrfs_warn_experimental("Feature: conversion to block-group-tree");
			to_bg_tree = true;
			break;
		case 'S':
			seeding_flag = 1;
			seeding_value = arg_strtou64(optarg);
			break;
		case 'r':
			super_flags |= BTRFS_FEATURE_INCOMPAT_EXTENDED_IREF;
			break;
		case 'x':
			super_flags |= BTRFS_FEATURE_INCOMPAT_SKINNY_METADATA;
			break;
		case 'n':
			super_flags |= BTRFS_FEATURE_INCOMPAT_NO_HOLES;
			break;
		case 'f':
			force = 1;
			break;
		case 'U':
			ctree_flags |= OPEN_CTREE_IGNORE_FSID_MISMATCH;
			new_fsid_str = optarg;
			break;
		case 'u':
			ctree_flags |= OPEN_CTREE_IGNORE_FSID_MISMATCH;
			random_fsid = 1;
			break;
		case 'M':
			ctree_flags |= OPEN_CTREE_IGNORE_FSID_MISMATCH;
			change_metadata_uuid = 1;
			new_fsid_str = optarg;
			break;
		case 'm':
			ctree_flags |= OPEN_CTREE_IGNORE_FSID_MISMATCH;
			change_metadata_uuid = 1;
			break;
#if EXPERIMENTAL
		case GETOPT_VAL_CSUM:
			btrfs_warn_experimental(
				"Switching checksums is experimental, do not use for valuable data!");
			ctree_flags |= OPEN_CTREE_SKIP_CSUM_CHECK;
			csum_type = parse_csum_type(optarg);
			break;
#endif
		case GETOPT_VAL_HELP:
		default:
			usage(&tune_cmd, c != GETOPT_VAL_HELP);
		}
	}

	set_argv0(argv);
	device = argv[optind];
	if (check_argc_exact(argc - optind, 1))
		return 1;

	if (random_fsid && new_fsid_str) {
		error("random fsid can't be used with specified fsid");
		return 1;
	}
	if (!super_flags && !seeding_flag && !(random_fsid || new_fsid_str) &&
	    !change_metadata_uuid && csum_type == -1 && !to_bg_tree) {
		error("at least one option should be specified");
		usage(&tune_cmd, 1);
		return 1;
	}

	if (new_fsid_str) {
		uuid_t tmp;

		ret = uuid_parse(new_fsid_str, tmp);
		if (ret < 0) {
			error("could not parse UUID: %s", new_fsid_str);
			return 1;
		}
		if (!test_uuid_unique(new_fsid_str)) {
			error("fsid %s is not unique", new_fsid_str);
			return 1;
		}
	}

	fd = open(device, O_RDWR);
	if (fd < 0) {
		error("mount check: cannot open %s: %m", device);
		return 1;
	}

	ret = check_mounted_where(fd, device, NULL, 0, NULL,
			SBREAD_IGNORE_FSID_MISMATCH);
	if (ret < 0) {
		errno = -ret;
		error("could not check mount status of %s: %m", device);
		close(fd);
		return 1;
	} else if (ret) {
		error("%s is mounted", device);
		close(fd);
		return 1;
	}

	root = open_ctree_fd(fd, device, 0, ctree_flags);

	if (!root) {
		error("open ctree failed");
		return 1;
	}

	if (to_bg_tree) {
		if (btrfs_fs_compat_ro(root->fs_info, BLOCK_GROUP_TREE)) {
			error("the filesystem already has block group tree feature");
			ret = 1;
			goto out;
		}
		if (!btrfs_fs_compat_ro(root->fs_info, FREE_SPACE_TREE_VALID)) {
			error("the filesystem doesn't have space cache v2, needs to be mounted with \"-o space_cache=v2\" first");
			ret = 1;
			goto out;
		}
		ret = convert_to_bg_tree(root->fs_info);
		if (ret < 0) {
			error("failed to convert the filesystem to block group tree feature");
			goto out;
		}
		goto out;
	}
	if (seeding_flag) {
		if (btrfs_fs_incompat(root->fs_info, METADATA_UUID)) {
			error("SEED flag cannot be changed on a metadata-uuid changed fs");
			ret = 1;
			goto out;
		}

		if (!seeding_value && !force) {
			warning(
"this is dangerous, clearing the seeding flag may cause the derived device not to be mountable!");
			ret = ask_user("We are going to clear the seeding flag, are you sure?");
			if (!ret) {
				error("clear seeding flag canceled");
				ret = 1;
				goto out;
			}
		}

		ret = update_seeding_flag(root, device, seeding_value, force);
		if (!ret)
			success++;
		total++;
	}

	if (super_flags) {
		ret = set_super_incompat_flags(root, super_flags);
		if (!ret)
			success++;
		total++;
	}

	if (csum_type != -1) {
		/* TODO: check conflicting flags */
		pr_verbose(LOG_DEFAULT, "Proceed to switch checksums\n");
		ret = rewrite_checksums(root->fs_info, csum_type);
	}

	if (change_metadata_uuid) {
		if (seeding_flag) {
			error("not allowed to set both seeding flag and uuid metadata");
			ret = 1;
			goto out;
		}

		if (new_fsid_str)
			ret = set_metadata_uuid(root, new_fsid_str);
		else
			ret = set_metadata_uuid(root, NULL);

		if (!ret)
			success++;
		total++;
	}

	if (random_fsid || (new_fsid_str && !change_metadata_uuid)) {
		if (btrfs_fs_incompat(root->fs_info, METADATA_UUID)) {
			error(
		"Cannot rewrite fsid while METADATA_UUID flag is active. \n"
		"Ensure fsid and metadata_uuid match before retrying.");
			ret = 1;
			goto out;
		}

		if (!force) {
			warning(
"it's recommended to run 'btrfs check --readonly' before this operation.\n"
"\tThe whole operation must finish before the filesystem can be mounted again.\n"
"\tIf cancelled or interrupted, run 'btrfstune -u' to restart.");
			ret = ask_user("We are going to change UUID, are your sure?");
			if (!ret) {
				error("UUID change canceled");
				ret = 1;
				goto out;
			}
		}
		ret = change_uuid(root->fs_info, new_fsid_str);
		if (!ret)
			success++;
		total++;
	}

	if (success == total) {
		ret = 0;
	} else {
		root->fs_info->readonly = 1;
		ret = 1;
		error("btrfstune failed");
	}
out:
	close_ctree(root);
	btrfs_close_all_devices();

	return ret;
}
