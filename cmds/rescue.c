/*
 * Copyright (C) 2013 SUSE.  All rights reserved.
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
#include <sys/stat.h>
#include <sys/sysmacros.h>
#include <getopt.h>
#include <errno.h>
#include <stdio.h>
#include "kernel-lib/list.h"
#include "kernel-shared/ctree.h"
#include "kernel-shared/volumes.h"
#include "kernel-shared/transaction.h"
#include "kernel-shared/disk-io.h"
#include "kernel-shared/extent_io.h"
#include "common/messages.h"
#include "common/utils.h"
#include "common/help.h"
#include "common/open-utils.h"
#include "cmds/commands.h"
#include "cmds/rescue.h"

static const char * const rescue_cmd_group_usage[] = {
	"btrfs rescue <command> [options] <path>",
	NULL
};

static const char * const cmd_rescue_chunk_recover_usage[] = {
	"btrfs rescue chunk-recover [options] <device>",
	"Recover the chunk tree by scanning the devices one by one.",
	"",
	OPTLINE("-y", "assume an answer of `yes' to all questions"),
	OPTLINE("-h", "help"),
	OPTLINE("-v", "deprecated, alias for global -v option"),
	HELPINFO_INSERT_GLOBALS,
	HELPINFO_INSERT_VERBOSE,
	NULL
};

static int cmd_rescue_chunk_recover(const struct cmd_struct *cmd,
				    int argc, char *argv[])
{
	int ret = 0;
	char *file;
	bool yes = false;

	/* If verbose is unset, set it to 0 */
	if (bconf.verbose == BTRFS_BCONF_UNSET)
		bconf.verbose = BTRFS_BCONF_QUIET;

	optind = 0;
	while (1) {
		int c = getopt(argc, argv, "yvh");
		if (c < 0)
			break;
		switch (c) {
		case 'y':
			yes = true;
			break;
		case 'v':
			bconf.verbose++;
			break;
		default:
			usage_unknown_option(cmd, argv);
		}
	}

	if (check_argc_exact(argc - optind, 1))
		return 1;

	file = argv[optind];

	ret = check_mounted(file);
	if (ret < 0) {
		errno = -ret;
		error("could not check mount status: %m");
		return 1;
	} else if (ret) {
		error("the device is busy");
		return 1;
	}

	ret = btrfs_recover_chunk_tree(file, yes);
	if (!ret) {
		pr_verbose(LOG_DEFAULT, "Chunk tree recovered successfully\n");
	} else if (ret > 0) {
		ret = 0;
		pr_verbose(LOG_DEFAULT, "Chunk tree recovery aborted\n");
	} else {
		pr_verbose(LOG_DEFAULT, "Chunk tree recovery failed\n");
	}
	return ret;
}
static DEFINE_SIMPLE_COMMAND(rescue_chunk_recover, "chunk-recover");

static const char * const cmd_rescue_super_recover_usage[] = {
	"btrfs rescue super-recover [options] <device>",
	"Recover bad superblocks from good copies",
	"",
	OPTLINE("-y", "assume an answer of `yes' to all questions"),
	OPTLINE("-v", "deprecated, alias for global -v option"),
	HELPINFO_INSERT_GLOBALS,
	HELPINFO_INSERT_VERBOSE,
	NULL
};

/*
 * return codes:
 *   0 : All superblocks are valid, no need to recover
 *   1 : Usage or syntax error
 *   2 : Recover all bad superblocks successfully
 *   3 : Fail to Recover bad superblocks
 *   4 : Abort to recover bad superblocks
 */
static int cmd_rescue_super_recover(const struct cmd_struct *cmd,
				    int argc, char **argv)
{
	int ret;
	bool yes = false;
	char *dname;

	optind = 0;
	while (1) {
		int c = getopt(argc, argv, "vy");
		if (c < 0)
			break;
		switch (c) {
		case 'v':
			bconf_be_verbose();
			break;
		case 'y':
			yes = true;
			break;
		default:
			usage_unknown_option(cmd, argv);
		}
	}
	if (check_argc_exact(argc - optind, 1))
		return 1;

	dname = argv[optind];
	ret = check_mounted(dname);
	if (ret < 0) {
		errno = -ret;
		error("could not check mount status: %m");
		return 1;
	} else if (ret) {
		error("the device is busy");
		return 1;
	}
	ret = btrfs_recover_superblocks(dname, yes);
	return ret;
}
static DEFINE_SIMPLE_COMMAND(rescue_super_recover, "super-recover");

static const char * const cmd_rescue_zero_log_usage[] = {
	"btrfs rescue zero-log <device>",
	"Clear the tree log. Usable if it's corrupted and prevents mount.",
	NULL
};

static int cmd_rescue_zero_log(const struct cmd_struct *cmd,
			       int argc, char **argv)
{
	struct btrfs_root *root;
	struct btrfs_super_block *sb;
	char *devname;
	int ret;

	clean_args_no_options(cmd, argc, argv);

	if (check_argc_exact(argc, 2))
		return 1;

	devname = argv[optind];
	ret = check_mounted(devname);
	if (ret < 0) {
		errno = -ret;
		error("could not check mount status: %m");
		goto out;
	} else if (ret) {
		error("%s is currently mounted", devname);
		ret = -EBUSY;
		goto out;
	}

	root = open_ctree(devname, 0, OPEN_CTREE_WRITES | OPEN_CTREE_PARTIAL |
			  OPEN_CTREE_NO_BLOCK_GROUPS);
	if (!root) {
		error("could not open ctree");
		return 1;
	}

	sb = root->fs_info->super_copy;
	pr_verbose(LOG_DEFAULT, "Clearing log on %s, previous log_root %llu, level %u\n",
			devname, btrfs_super_log_root(sb),
			(unsigned)btrfs_super_log_root_level(sb));
	btrfs_set_super_log_root(sb, 0);
	btrfs_set_super_log_root_level(sb, 0);
	ret = write_all_supers(root->fs_info);
	if (ret < 0) {
		errno = -ret;
		error("failed to write dev supers: %m");
	}
	close_ctree(root);
out:
	return !!ret;
}
static DEFINE_SIMPLE_COMMAND(rescue_zero_log, "zero-log");

static const char * const cmd_rescue_fix_device_size_usage[] = {
	"btrfs rescue fix-device-size <device>",
	"Re-align device and super block sizes. Usable if newer kernel refuse to mount it due to mismatch super size",
	NULL
};

static int cmd_rescue_fix_device_size(const struct cmd_struct *cmd,
				      int argc, char **argv)
{
	struct btrfs_fs_info *fs_info;
	struct open_ctree_flags ocf = { 0 };
	char *devname;
	int ret;

	clean_args_no_options(cmd, argc, argv);

	if (check_argc_exact(argc, 2))
		return 1;

	devname = argv[optind];
	ret = check_mounted(devname);
	if (ret < 0) {
		errno = -ret;
		error("could not check mount status: %m");
		goto out;
	} else if (ret) {
		error("%s is currently mounted", devname);
		ret = -EBUSY;
		goto out;
	}

	ocf.filename = devname;
	ocf.flags = OPEN_CTREE_WRITES | OPEN_CTREE_PARTIAL;
	fs_info = open_ctree_fs_info(&ocf);
	if (!fs_info) {
		error("could not open btrfs");
		ret = -EIO;
		goto out;
	}

	ret = btrfs_fix_device_and_super_size(fs_info);
	if (ret > 0)
		ret = 0;
	close_ctree(fs_info->tree_root);
out:
	return !!ret;
}
static DEFINE_SIMPLE_COMMAND(rescue_fix_device_size, "fix-device-size");

static const char * const cmd_rescue_create_control_device_usage[] = {
	"btrfs rescue create-control-device",
	"Create /dev/btrfs-control (see 'CONTROL DEVICE' in btrfs(5))",
	NULL
};

static int cmd_rescue_create_control_device(const struct cmd_struct *cmd,
					    int argc, char **argv)
{
	dev_t device;
	int ret;

	if (check_argc_exact(argc, 1))
		return 1;

	device = makedev(10, 234);

	ret = mknod("/dev/btrfs-control", S_IFCHR | S_IRUSR | S_IWUSR, device);
	if (ret) {
		error("could not create /dev/btrfs-control: %m");
		return 1;
	}

	return 0;

}
static DEFINE_SIMPLE_COMMAND(rescue_create_control_device, "create-control-device");

static int clear_uuid_tree(struct btrfs_fs_info *fs_info)
{
	struct btrfs_root *uuid_root = fs_info->uuid_root;
	struct btrfs_trans_handle *trans;
	struct btrfs_path path = {};
	struct btrfs_key key = {};
	int ret;

	if (!uuid_root)
		return 0;

	fs_info->uuid_root = NULL;
	trans = btrfs_start_transaction(fs_info->tree_root, 0);
	if (IS_ERR(trans))
		return PTR_ERR(trans);

	while (1) {
		int nr;

		ret = btrfs_search_slot(trans, uuid_root, &key, &path, -1, 1);
		if (ret < 0)
			goto out;
		ASSERT(ret > 0);
		ASSERT(path.slots[0] == 0);

		nr = btrfs_header_nritems(path.nodes[0]);
		if (nr == 0) {
			btrfs_release_path(&path);
			break;
		}

		ret = btrfs_del_items(trans, uuid_root, &path, 0, nr);
		btrfs_release_path(&path);
		if (ret < 0)
			goto out;
	}
	ret = btrfs_del_root(trans, fs_info->tree_root, &uuid_root->root_key);
	if (ret < 0)
		goto out;
	list_del(&uuid_root->dirty_list);
	ret = clean_tree_block(uuid_root->node);
	if (ret < 0)
		goto out;
	ret = btrfs_free_tree_block(trans, uuid_root, uuid_root->node, 0, 1);
	if (ret < 0)
		goto out;
	free_extent_buffer(uuid_root->node);
	free_extent_buffer(uuid_root->commit_root);
	kfree(uuid_root);
out:
	if (ret < 0)
		btrfs_abort_transaction(trans, ret);
	else
		ret = btrfs_commit_transaction(trans, fs_info->tree_root);
	return ret;
}

static const char * const cmd_rescue_clear_uuid_tree_usage[] = {
	"btrfs rescue clear-uuid-tree",
	"Delete uuid tree so that kernel can rebuild it at mount time",
	NULL,
};

static int cmd_rescue_clear_uuid_tree(const struct cmd_struct *cmd,
				      int argc, char **argv)
{
	struct btrfs_fs_info *fs_info;
	struct open_ctree_flags ocf = {};
	char *devname;
	int ret;

	clean_args_no_options(cmd, argc, argv);
	if (check_argc_exact(argc, 2))
		return -EINVAL;

	devname = argv[optind];
	ret = check_mounted(devname);
	if (ret < 0) {
		errno = -ret;
		error("could not check mount status: %m");
		goto out;
	} else if (ret) {
		error("%s is currently mounted", devname);
		ret = -EBUSY;
		goto out;
	}
	ocf.filename = devname;
	ocf.flags = OPEN_CTREE_WRITES | OPEN_CTREE_PARTIAL;
	fs_info = open_ctree_fs_info(&ocf);
	if (!fs_info) {
		error("could not open btrfs");
		ret = -EIO;
		goto out;
	}

	ret = clear_uuid_tree(fs_info);
	close_ctree(fs_info->tree_root);
out:
	return !!ret;
}
static DEFINE_SIMPLE_COMMAND(rescue_clear_uuid_tree, "clear-uuid-tree");

static const char rescue_cmd_group_info[] =
"toolbox for specific rescue operations";

static const struct cmd_group rescue_cmd_group = {
	rescue_cmd_group_usage, rescue_cmd_group_info, {
		&cmd_struct_rescue_chunk_recover,
		&cmd_struct_rescue_super_recover,
		&cmd_struct_rescue_zero_log,
		&cmd_struct_rescue_fix_device_size,
		&cmd_struct_rescue_create_control_device,
		&cmd_struct_rescue_clear_uuid_tree,
		NULL
	}
};

DEFINE_GROUP_COMMAND_TOKEN(rescue);
