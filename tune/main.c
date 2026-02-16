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
#include <fcntl.h>
#include <unistd.h>
#include <getopt.h>
#include <errno.h>
#include <stdbool.h>
#include <uuid/uuid.h>
#include "kernel-shared/accessors.h"
#include "kernel-shared/ctree.h"
#include "kernel-shared/disk-io.h"
#include "kernel-shared/transaction.h"
#include "kernel-shared/volumes.h"
#include "kernel-shared/free-space-tree.h"
#include "kernel-shared/zoned.h"
#include "crypto/hash.h"
#include "common/cpu-utils.h"
#include "common/utils.h"
#include "common/open-utils.h"
#include "common/device-scan.h"
#include "common/messages.h"
#include "common/parse-utils.h"
#include "common/string-utils.h"
#include "common/help.h"
#include "common/box.h"
#include "common/clear-cache.h"
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

static int convert_to_fst(struct btrfs_fs_info *fs_info)
{
	int ret;

	/* We may have invalid old v2 cache, clear them first. */
	if (btrfs_fs_compat_ro(fs_info, FREE_SPACE_TREE)) {
		ret = btrfs_clear_free_space_tree(fs_info);
		if (ret < 0) {
			errno = -ret;
			error("failed to clear stale v2 free space cache: %m");
			return ret;
		}
	}
	ret = btrfs_clear_v1_cache(fs_info);
	if (ret < 0) {
		errno = -ret;
		error("failed to clear v1 free space cache: %m");
		return ret;
	}

	ret = btrfs_create_free_space_tree(fs_info);
	if (ret < 0) {
		errno = -ret;
		error("failed to create free space tree: %m");
		return ret;
	}
	pr_verbose(LOG_DEFAULT, "Converted to free space tree feature\n");
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
	OPTLINE("--enable-simple-quota", "enable simple quotas on the file system. (mkfs: squota)"),
	OPTLINE("--remove-simple-quota", "remove simple quotas from the file system."),
	OPTLINE("--convert-to-block-group-tree", "convert filesystem to track block groups in "
			"the separate block-group-tree instead of extent tree (sets the incompat bit)"),
	OPTLINE("--convert-from-block-group-tree",
			"convert the block group tree back to extent tree (remove the incompat bit)"),
	OPTLINE("--convert-to-free-space-tree", "convert filesystem to use free space tree (v2 cache)"),
	"",
	"UUID changes:",
	OPTLINE("-u", "rewrite fsid, use a random one"),
	OPTLINE("-U UUID", "rewrite fsid to UUID"),
	OPTLINE("-m", "change fsid to a random UUID, copy original fsid into "
		      "metadata_uuid if it's not NULL, this is an incompat change "
		      "(more lightweight than -u|-U)"),
	OPTLINE("-M UUID", "change fsid to UUID, copy original fsid into "
		      "metadata_uuid if it's not NULL, this is an incompat change "
		      "(more lightweight than -u|-U)"),
	"",
	"General:",
	OPTLINE("-f", "allow dangerous operations, make sure that you are aware of the dangers"),
	OPTLINE("--version", "print the btrfstune version, builtin features and exit"),
	OPTLINE("--help", "print this help and exit"),
#if EXPERIMENTAL
	"",
	"EXPERIMENTAL FEATURES:",
	OPTLINE("--csum CSUM", "switch checksum for data and metadata to CSUM"),
	OPTLINE("--convert-to-remap-tree", "convert filesystem to use the remap tree"),
#endif
	NULL
};

enum btrfstune_group_enum {
	/* Extent/block group tree feature. */
	EXTENT_TREE,

	/* V1/v2 free space cache. */
	SPACE_CACHE,

	/* Metadata UUID. */
	METADATA_UUID,

	/* FSID change. */
	FSID_CHANGE,

	/* Seed device. */
	SEED,

	/* Csum conversion */
	CSUM_CHANGE,

	/*
	 * Legacy features (which later become default), including:
	 * - no-holes
	 * - extref
	 * - skinny-metadata
	 */
	LEGACY,

	/* Qgroup options */
	QGROUP,

	/* Remap tree. */
	REMAP_TREE,

	BTRFSTUNE_NR_GROUPS,
};

static bool btrfstune_cmd_groups[BTRFSTUNE_NR_GROUPS] = { 0 };

static unsigned int btrfstune_count_set_groups(void)
{
	int ret = 0;

	for (int i = 0; i < BTRFSTUNE_NR_GROUPS; i++) {
		if (btrfstune_cmd_groups[i])
			ret++;
	}
	return ret;
}

static const struct cmd_struct tune_cmd = {
	.usagestr = tune_usage
};

int BOX_MAIN(btrfstune)(int argc, char *argv[])
{
	struct btrfs_root *root;
	struct btrfs_fs_info *fs_info;
	unsigned ctree_flags = OPEN_CTREE_WRITES | OPEN_CTREE_EXCLUSIVE;
	int seeding_flag = 0;
	u64 seeding_value = 0;
	int random_fsid = 0;
	int change_metadata_uuid = 0;
	bool to_extent_tree = false;
	bool to_bg_tree = false;
	bool to_fst = false;
	bool to_remap_tree = false;
	int csum_type = -1;
	char *new_fsid_str = NULL;
	int ret;
	u64 super_flags = 0;
	int quota = 0;
	int remove_simple_quota = 0;
	int fd = -1;
	int oflags = O_RDWR;

	cpu_detect_flags();
	hash_init_accel();
	btrfs_config_init();

	while(1) {
		enum { GETOPT_VAL_CSUM = GETOPT_VAL_FIRST,
		       GETOPT_VAL_ENABLE_BLOCK_GROUP_TREE,
		       GETOPT_VAL_DISABLE_BLOCK_GROUP_TREE,
		       GETOPT_VAL_ENABLE_FREE_SPACE_TREE,
		       GETOPT_VAL_ENABLE_SIMPLE_QUOTA,
		       GETOPT_VAL_REMOVE_SIMPLE_QUOTA,
		       GETOPT_VAL_ENABLE_REMAP_TREE,
		       GETOPT_VAL_VERSION,
		};
		static const struct option long_options[] = {
			{ "help", no_argument, NULL, GETOPT_VAL_HELP},
			{ "version", no_argument, NULL, GETOPT_VAL_VERSION },
			{ "convert-to-block-group-tree", no_argument, NULL,
				GETOPT_VAL_ENABLE_BLOCK_GROUP_TREE},
			{ "convert-from-block-group-tree", no_argument, NULL,
				GETOPT_VAL_DISABLE_BLOCK_GROUP_TREE},
			{ "convert-to-free-space-tree", no_argument, NULL,
				GETOPT_VAL_ENABLE_FREE_SPACE_TREE},
			{ "enable-simple-quota", no_argument, NULL,
				GETOPT_VAL_ENABLE_SIMPLE_QUOTA },
			{ "remove-simple-quota", no_argument, NULL,
				GETOPT_VAL_REMOVE_SIMPLE_QUOTA},
#if EXPERIMENTAL
			{ "csum", required_argument, NULL, GETOPT_VAL_CSUM },
			{ "convert-to-remap-tree", no_argument, NULL,
				GETOPT_VAL_ENABLE_REMAP_TREE},
#endif
			{ NULL, 0, NULL, 0 }
		};
		int c = getopt_long(argc, argv, "S:rxfuU:nmM:", long_options, NULL);

		if (c < 0)
			break;
		switch(c) {
		case 'S':
			seeding_flag = 1;
			seeding_value = arg_strtou64(optarg);
			btrfstune_cmd_groups[SEED] = true;
			break;
		case 'r':
			super_flags |= BTRFS_FEATURE_INCOMPAT_EXTENDED_IREF;
			btrfstune_cmd_groups[LEGACY] = true;
			break;
		case 'x':
			super_flags |= BTRFS_FEATURE_INCOMPAT_SKINNY_METADATA;
			btrfstune_cmd_groups[LEGACY] = true;
			break;
		case 'n':
			super_flags |= BTRFS_FEATURE_INCOMPAT_NO_HOLES;
			btrfstune_cmd_groups[LEGACY] = true;
			break;
		case 'f':
			force = 1;
			break;
		case 'U':
			ctree_flags |= OPEN_CTREE_IGNORE_FSID_MISMATCH;
			new_fsid_str = optarg;
			btrfstune_cmd_groups[FSID_CHANGE] = true;
			break;
		case 'u':
			ctree_flags |= OPEN_CTREE_IGNORE_FSID_MISMATCH;
			random_fsid = 1;
			btrfstune_cmd_groups[FSID_CHANGE] = true;
			break;
		case 'M':
			ctree_flags |= OPEN_CTREE_IGNORE_FSID_MISMATCH;
			change_metadata_uuid = 1;
			new_fsid_str = optarg;
			btrfstune_cmd_groups[METADATA_UUID] = true;
			break;
		case 'm':
			ctree_flags |= OPEN_CTREE_IGNORE_FSID_MISMATCH;
			change_metadata_uuid = 1;
			btrfstune_cmd_groups[METADATA_UUID] = true;
			break;
		case GETOPT_VAL_ENABLE_BLOCK_GROUP_TREE:
			to_bg_tree = true;
			btrfstune_cmd_groups[EXTENT_TREE] = true;
			break;
		case GETOPT_VAL_DISABLE_BLOCK_GROUP_TREE:
			to_extent_tree = true;
			btrfstune_cmd_groups[EXTENT_TREE] = true;
			break;
		case GETOPT_VAL_ENABLE_FREE_SPACE_TREE:
			to_fst = true;
			btrfstune_cmd_groups[SPACE_CACHE] = true;
			break;
		case GETOPT_VAL_ENABLE_SIMPLE_QUOTA:
			quota = 1;
			btrfstune_cmd_groups[QGROUP] = true;
			break;
		case GETOPT_VAL_REMOVE_SIMPLE_QUOTA:
			remove_simple_quota = 1;
			btrfstune_cmd_groups[QGROUP] = true;
			break;
#if EXPERIMENTAL
		case GETOPT_VAL_CSUM:
			btrfs_warn_experimental(
				"Switching checksums is experimental, do not use for valuable data!");
			ctree_flags |= OPEN_CTREE_SKIP_CSUM_CHECK;
			csum_type = parse_csum_type(optarg);
			btrfstune_cmd_groups[CSUM_CHANGE] = true;
			break;
		case GETOPT_VAL_ENABLE_REMAP_TREE:
			to_remap_tree = true;
			btrfstune_cmd_groups[REMAP_TREE] = true;
			break;
#endif
		case GETOPT_VAL_VERSION:
			help_builtin_features("btrfstune, part of ");
			ret = 0;
			goto free_out;
		case GETOPT_VAL_HELP:
		default:
			usage(&tune_cmd, c != GETOPT_VAL_HELP);
		}
	}

	set_argv0(argv);
	device = argv[optind];
	if (check_argc_exact(argc - optind, 1)) {
		ret = 1;
		goto free_out;
	}

	if (btrfstune_count_set_groups() == 0) {
		error("at least one option should be specified");
		usage(&tune_cmd, 1);
		ret = 1;
		goto free_out;
	}
	if (btrfstune_count_set_groups() > 1) {
		error("too many conflicting options specified");
		usage(&tune_cmd, 1);
		ret = 1;
		goto free_out;
	}
	if (random_fsid && new_fsid_str) {
		error("random fsid can't be used with specified fsid");
		ret = 1;
		goto free_out;
	}
	if (new_fsid_str) {
		uuid_t tmp;

		ret = uuid_parse(new_fsid_str, tmp);
		if (ret < 0) {
			error("could not parse UUID: %s", new_fsid_str);
			ret = 1;
			goto free_out;
		}
		if (!test_uuid_unique(new_fsid_str)) {
			error("fsid %s is not unique", new_fsid_str);
			ret = 1;
			goto free_out;
		}
	}

	if (zoned_model(device) == ZONED_HOST_MANAGED)
		oflags |= O_DIRECT;
	fd = open(device, oflags);
	if (fd < 0) {
		error("mount check: cannot open %s: %m", device);
		ret = 1;
		goto free_out;
	}

	ret = check_mounted_where(fd, device, NULL, 0, NULL,
				  SBREAD_IGNORE_FSID_MISMATCH, false);
	if (ret < 0) {
		errno = -ret;
		error("could not check mount status of %s: %m", device);
		close(fd);
		ret = 1;
		goto free_out;
	} else if (ret) {
		error("%s is mounted", device);
		close(fd);
		ret = 1;
		goto free_out;
	}

	/*
	 * For fsid changes we must use the latest device (not necessarily the
	 * one specified on command line so the matching of the device
	 * belonging to the filesystem works.
	 */
	if (change_metadata_uuid || random_fsid || new_fsid_str)
		ctree_flags |= OPEN_CTREE_USE_LATEST_BDEV;

	root = open_ctree_fd(fd, device, 0, ctree_flags);

	if (!root) {
		error("open ctree failed");
		ret = 1;
		goto free_out;
	}
	fs_info = root->fs_info;

	/*
	 * As we increment the generation number here, it is unlikely that the
	 * missing device will have a higher generation number, and the kernel
	 * won't use its super block for any further commits, even if it is not
	 * missing during mount.
	 *
	 * So, we allow all operations except for -m, -M, -u, and -U, as these
	 * operations also change the fsid/metadata_uuid, which are key
	 * parameters for assembling the devices and need to be consistent on
	 * all the partner devices.
	 */
	if ((change_metadata_uuid || random_fsid || new_fsid_str) &&
	     fs_info->fs_devices->missing_devices > 0) {
		error("missing %lld device(s), failing the command",
		       fs_info->fs_devices->missing_devices);
		ret = 1;
		goto out;
	}

	if (to_remap_tree) {
		if (!btrfs_fs_compat_ro(fs_info, BLOCK_GROUP_TREE)) {
			if (to_extent_tree) {
				error("remap tree option depends on the block-group tree");
				ret = -EINVAL;
				goto out;
			} else {
				printf("remap tree depends on block-group tree, enabling that also\n");
				to_bg_tree = true;
			}
		}

		if (!btrfs_fs_compat_ro(fs_info, FREE_SPACE_TREE_VALID)) {
			printf("remap tree depends on free-space tree, enabling that also\n");
			to_fst = true;
		}
	}

 	if (to_bg_tree) {
		if (to_extent_tree) {
			error("option --convert-to-block-group-tree conflicts with --convert-from-block-group-tree");
			ret = -EINVAL;
			goto out;
		}
		if (btrfs_fs_compat_ro(fs_info, BLOCK_GROUP_TREE)) {
			error("the filesystem already has block group tree feature");
			ret = -EINVAL;
			goto out;
		}
		if (!btrfs_fs_compat_ro(fs_info, FREE_SPACE_TREE_VALID)) {
			error("the filesystem doesn't have space cache v2, needs to be mounted with \"-o space_cache=v2\" first");
			ret = -EINVAL;
			goto out;
		}
		ret = convert_to_bg_tree(fs_info);
		if (ret < 0) {
			error("failed to convert the filesystem to block group tree feature");
			goto out;
		}

		if (!to_remap_tree)
			goto out;
	}
	if (to_fst) {
		if (btrfs_fs_compat_ro(fs_info, FREE_SPACE_TREE_VALID)) {
			error("filesystem already has free-space-tree feature");
			ret = -EINVAL;
			goto out;
		}
		ret = convert_to_fst(fs_info);
		if (ret < 0)
			error("failed to convert the filesystem to free-space-tree feature");

		if (!to_remap_tree)
			goto out;
	}
	if (to_extent_tree) {
		if (to_bg_tree) {
			error("option --convert-to-block-group-tree conflicts with --convert-from-block-group-tree");
			ret = -EINVAL;
			goto out;
		}
		if (!btrfs_fs_compat_ro(fs_info, BLOCK_GROUP_TREE)) {
			error("filesystem doesn't have block-group-tree feature");
			ret = -EINVAL;
			goto out;
		}
		ret = convert_to_extent_tree(fs_info);
		if (ret < 0) {
			error("failed to convert the filesystem from block group tree feature");
			goto out;
		}
		goto out;
	}
	if (seeding_flag) {
		if (btrfs_fs_incompat(fs_info, METADATA_UUID)) {
			error("SEED flag cannot be changed on a metadata-uuid changed fs");
			ret = -EINVAL;
			goto out;
		}

		if (!seeding_value && !force) {
			warning(
"this is dangerous, clearing the seeding flag may cause the derived device not to be mountable!");
			ret = ask_user("We are going to clear the seeding flag, are you sure?");
			if (!ret) {
				error("clear seeding flag canceled");
				ret = -EINVAL;
				goto out;
			}
		}

		ret = update_seeding_flag(root, device, seeding_value, force);
		goto out;
	}
	if (to_remap_tree) {
		if (btrfs_fs_incompat(fs_info, REMAP_TREE)) {
			error("filesystem already has remap-tree feature");
			ret = -EINVAL;
			goto out;
		}
		ret = convert_to_remap_tree(fs_info);
		if (ret < 0)
			error("failed to convert the filesystem to remap-tree feature");
		goto out;
	}

	if (super_flags) {
		ret = set_super_incompat_flags(root, super_flags);
		goto out;
	}

	if (csum_type != -1) {
		pr_verbose(LOG_DEFAULT, "Proceed to switch checksums\n");
		ret = btrfs_change_csum_type(fs_info, csum_type);
		goto out;
	}

	if (change_metadata_uuid) {
		if (seeding_flag) {
			error("not allowed to set both seeding flag and uuid metadata");
			ret = -EINVAL;
			goto out;
		}

		if (new_fsid_str)
			ret = set_metadata_uuid(root, new_fsid_str);
		else
			ret = set_metadata_uuid(root, NULL);

		btrfs_register_all_devices();
		goto out;
	}

	if (random_fsid || (new_fsid_str && !change_metadata_uuid)) {
		if (fs_info->fs_devices->active_metadata_uuid) {
			error(
		"Cannot rewrite fsid while METADATA_UUID flag is active. \n"
		"Ensure fsid and metadata_uuid match before retrying.");
			ret = -EINVAL;
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
				ret = -EINVAL;
				goto out;
			}
		}
		ret = change_uuid(fs_info, new_fsid_str);
		goto out;
	}

	if (quota) {
		ret = enable_quota(root->fs_info, true);
		if (ret)
			goto out;
	}

	if (remove_simple_quota) {
		ret = remove_squota(root->fs_info);
		if (ret)
			goto out;
	}

out:
	if (ret < 0) {
		fs_info->readonly = 1;
		ret = 1;
		error("btrfstune failed");
	}
	close_ctree(root);
	btrfs_close_all_devices();
	close(fd);

free_out:
	return ret;
}
