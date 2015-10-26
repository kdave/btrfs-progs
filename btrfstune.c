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

#include <stdio.h>
#include <stdlib.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <unistd.h>
#include <dirent.h>
#include <uuid/uuid.h>
#include <getopt.h>

#include "kerncompat.h"
#include "ctree.h"
#include "disk-io.h"
#include "transaction.h"
#include "utils.h"
#include "volumes.h"

static char *device;
static int force = 0;

static int update_seeding_flag(struct btrfs_root *root, int set_flag)
{
	struct btrfs_trans_handle *trans;
	struct btrfs_super_block *disk_super;
	u64 super_flags;
	int ret;

	disk_super = root->fs_info->super_copy;
	super_flags = btrfs_super_flags(disk_super);
	if (set_flag) {
		if (super_flags & BTRFS_SUPER_FLAG_SEEDING) {
			if (force)
				return 0;
			else
				fprintf(stderr, "seeding flag is already set on %s\n", device);
			return 1;
		}
		super_flags |= BTRFS_SUPER_FLAG_SEEDING;
	} else {
		if (!(super_flags & BTRFS_SUPER_FLAG_SEEDING)) {
			fprintf(stderr, "seeding flag is not set on %s\n",
				device);
			return 1;
		}
		super_flags &= ~BTRFS_SUPER_FLAG_SEEDING;
		fprintf(stderr, "Warning: Seeding flag cleared.\n");
	}

	trans = btrfs_start_transaction(root, 1);
	btrfs_set_super_flags(disk_super, super_flags);
	ret = btrfs_commit_transaction(trans, root);

	return ret;
}

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
	btrfs_set_super_incompat_flags(disk_super, super_flags);
	ret = btrfs_commit_transaction(trans, root);

	return ret;
}

static int change_header_uuid(struct btrfs_root *root, struct extent_buffer *eb)
{
	struct btrfs_fs_info *fs_info = root->fs_info;
	int same_fsid = 1;
	int same_chunk_tree_uuid = 1;
	int ret;

	same_fsid = !memcmp_extent_buffer(eb, fs_info->new_fsid,
			btrfs_header_fsid(), BTRFS_FSID_SIZE);
	same_chunk_tree_uuid =
		!memcmp_extent_buffer(eb, fs_info->new_chunk_tree_uuid,
				btrfs_header_chunk_tree_uuid(eb),
				BTRFS_UUID_SIZE);
	if (same_fsid && same_chunk_tree_uuid)
		return 0;
	if (!same_fsid)
		write_extent_buffer(eb, fs_info->new_fsid, btrfs_header_fsid(),
				    BTRFS_FSID_SIZE);
	if (!same_chunk_tree_uuid)
		write_extent_buffer(eb, fs_info->new_chunk_tree_uuid,
				    btrfs_header_chunk_tree_uuid(eb),
				    BTRFS_UUID_SIZE);
	ret = write_tree_block(NULL, root, eb);

	return ret;
}

static int change_extents_uuid(struct btrfs_fs_info *fs_info)
{
	struct btrfs_root *root = fs_info->extent_root;
	struct btrfs_path *path;
	struct btrfs_key key = {0, 0, 0};
	int ret = 0;

	path = btrfs_alloc_path();
	if (!path)
		return -ENOMEM;

	/*
	 * Here we don't use transaction as it will takes a lot of reserve
	 * space, and that will make a near-full btrfs unable to change uuid
	 */
	ret = btrfs_search_slot(NULL, root, &key, path, 0, 0);
	if (ret < 0)
		goto out;

	while (1) {
		struct btrfs_extent_item *ei;
		struct extent_buffer *eb;
		u64 flags;
		u64 bytenr;

		btrfs_item_key_to_cpu(path->nodes[0], &key, path->slots[0]);
		if (key.type != BTRFS_EXTENT_ITEM_KEY &&
		    key.type != BTRFS_METADATA_ITEM_KEY)
			goto next;
		ei = btrfs_item_ptr(path->nodes[0], path->slots[0],
				    struct btrfs_extent_item);
		flags = btrfs_extent_flags(path->nodes[0], ei);
		if (!(flags & BTRFS_EXTENT_FLAG_TREE_BLOCK))
			goto next;

		bytenr = key.objectid;
		eb = read_tree_block(root, bytenr, root->nodesize, 0);
		if (IS_ERR(eb)) {
			fprintf(stderr, "Failed to read tree block: %llu\n",
				bytenr);
			ret = PTR_ERR(eb);
			goto out;
		}
		ret = change_header_uuid(root, eb);
		free_extent_buffer(eb);
		if (ret < 0) {
			fprintf(stderr, "Failed to change uuid of tree block: %llu\n",
				bytenr);
			goto out;
		}
next:
		ret = btrfs_next_item(root, path);
		if (ret < 0)
			goto out;
		if (ret > 0) {
			ret = 0;
			goto out;
		}
	}

out:
	btrfs_free_path(path);
	return ret;
}

static int change_device_uuid(struct btrfs_root *root, struct extent_buffer *eb,
			      int slot)
{
	struct btrfs_fs_info *fs_info = root->fs_info;
	struct btrfs_dev_item *di;
	int ret = 0;

	di = btrfs_item_ptr(eb, slot, struct btrfs_dev_item);
	if (!memcmp_extent_buffer(eb, fs_info->new_fsid,
				  (unsigned long)btrfs_device_fsid(di),
				  BTRFS_FSID_SIZE))
		return ret;

	write_extent_buffer(eb, fs_info->new_fsid,
			    (unsigned long)btrfs_device_fsid(di),
			    BTRFS_FSID_SIZE);
	ret = write_tree_block(NULL, root, eb);

	return ret;
}

static int change_devices_uuid(struct btrfs_fs_info *fs_info)
{
	struct btrfs_root *root = fs_info->chunk_root;
	struct btrfs_path *path;
	struct btrfs_key key = {0, 0, 0};
	int ret = 0;

	path = btrfs_alloc_path();
	if (!path)
		return -ENOMEM;
	/* No transaction again */
	ret = btrfs_search_slot(NULL, root, &key, path, 0, 0);
	if (ret < 0)
		goto out;

	while (1) {
		btrfs_item_key_to_cpu(path->nodes[0], &key, path->slots[0]);
		if (key.type != BTRFS_DEV_ITEM_KEY ||
		    key.objectid != BTRFS_DEV_ITEMS_OBJECTID)
			goto next;
		ret = change_device_uuid(root, path->nodes[0], path->slots[0]);
		if (ret < 0)
			goto out;
next:
		ret = btrfs_next_item(root, path);
		if (ret < 0)
			goto out;
		if (ret > 0) {
			ret = 0;
			goto out;
		}
	}
out:
	btrfs_free_path(path);
	return ret;
}

static int change_fsid_prepare(struct btrfs_fs_info *fs_info)
{
	struct btrfs_root *tree_root = fs_info->tree_root;
	u64 flags = btrfs_super_flags(fs_info->super_copy);
	int ret = 0;

	flags |= BTRFS_SUPER_FLAG_CHANGING_FSID;
	btrfs_set_super_flags(fs_info->super_copy, flags);

	memcpy(fs_info->super_copy->fsid, fs_info->new_fsid, BTRFS_FSID_SIZE);
	ret = write_all_supers(tree_root);
	if (ret < 0)
		return ret;

	/* also restore new chunk_tree_id into tree_root for restore */
	write_extent_buffer(tree_root->node, fs_info->new_chunk_tree_uuid,
			    btrfs_header_chunk_tree_uuid(tree_root->node),
			    BTRFS_UUID_SIZE);
	return write_tree_block(NULL, tree_root, tree_root->node);
}

static int change_fsid_done(struct btrfs_fs_info *fs_info)
{
	u64 flags = btrfs_super_flags(fs_info->super_copy);

	flags &= ~BTRFS_SUPER_FLAG_CHANGING_FSID;
	btrfs_set_super_flags(fs_info->super_copy, flags);

	return write_all_supers(fs_info->tree_root);
}

/*
 * Return 0 for no unfinished fsid change.
 * Return >0 for unfinished fsid change, and restore unfinished fsid/
 * chunk_tree_id into fsid_ret/chunk_id_ret.
 */
static int check_unfinished_fsid_change(struct btrfs_fs_info *fs_info,
					uuid_t fsid_ret, uuid_t chunk_id_ret)
{
	struct btrfs_root *tree_root = fs_info->tree_root;
	u64 flags = btrfs_super_flags(fs_info->super_copy);

	if (flags & BTRFS_SUPER_FLAG_CHANGING_FSID) {
		memcpy(fsid_ret, fs_info->super_copy->fsid, BTRFS_FSID_SIZE);
		read_extent_buffer(tree_root->node, chunk_id_ret,
				btrfs_header_chunk_tree_uuid(tree_root->node),
				BTRFS_UUID_SIZE);
		return 1;
	}
	return 0;
}

/*
 * Change fsid of a given fs.
 *
 * If new_fsid_str is not given, use a random generated UUID.
 * Caller should check new_fsid_str is valid
 */
static int change_uuid(struct btrfs_fs_info *fs_info, const char *new_fsid_str)
{
	uuid_t new_fsid;
	uuid_t new_chunk_id;
	uuid_t old_fsid;
	char uuid_buf[BTRFS_UUID_UNPARSED_SIZE];
	int ret = 0;

	if (check_unfinished_fsid_change(fs_info, new_fsid, new_chunk_id)) {
		if (new_fsid_str) {
			uuid_t tmp;

			uuid_parse(new_fsid_str, tmp);
			if (memcmp(tmp, new_fsid, BTRFS_FSID_SIZE)) {
				fprintf(stderr,
		"ERROR: New fsid %s is not the same with unfinished fsid change\n",
					new_fsid_str);
				return -EINVAL;
			}
		}
	} else {
		if (new_fsid_str)
			uuid_parse(new_fsid_str, new_fsid);
		else
			uuid_generate(new_fsid);

		uuid_generate(new_chunk_id);
	}
	fs_info->new_fsid = new_fsid;
	fs_info->new_chunk_tree_uuid = new_chunk_id;

	memcpy(old_fsid, (const char*)fs_info->fsid, BTRFS_UUID_SIZE);
	uuid_unparse(old_fsid, uuid_buf);
	printf("Current fsid: %s\n", uuid_buf);

	uuid_unparse(new_fsid, uuid_buf);
	printf("New fsid: %s\n", uuid_buf);
	/* Now we can begin fsid change */
	printf("Set superblock flag CHANGING_FSID\n");
	ret = change_fsid_prepare(fs_info);
	if (ret < 0)
		goto out;

	/* Change extents first */
	printf("Change fsid in extents\n");
	ret = change_extents_uuid(fs_info);
	if (ret < 0) {
		fprintf(stderr, "Failed to change UUID of metadata\n");
		goto out;
	}

	/* Then devices */
	printf("Change fsid on devices\n");
	ret = change_devices_uuid(fs_info);
	if (ret < 0) {
		fprintf(stderr, "Failed to change UUID of devices\n");
		goto out;
	}

	/* Last, change fsid in super */
	memcpy(fs_info->fs_devices->fsid, fs_info->new_fsid,
	       BTRFS_FSID_SIZE);
	memcpy(fs_info->super_copy->fsid, fs_info->new_fsid,
	       BTRFS_FSID_SIZE);
	ret = write_all_supers(fs_info->tree_root);
	if (ret < 0)
		goto out;

	/* Now fsid change is done */
	printf("Clear superblock flag CHANGING_FSID\n");
	ret = change_fsid_done(fs_info);
	fs_info->new_fsid = NULL;
	fs_info->new_chunk_tree_uuid = NULL;
	printf("Fsid change finished\n");
out:
	return ret;
}

static void print_usage(void)
{
	fprintf(stderr, "usage: btrfstune [options] device\n");
	fprintf(stderr, "\t-S value\tpositive value will enable seeding, zero to disable, negative is not allowed\n");
	fprintf(stderr, "\t-r \t\tenable extended inode refs\n");
	fprintf(stderr, "\t-x \t\tenable skinny metadata extent refs\n");
	fprintf(stderr, "\t-n \t\tenable no-holes feature (more efficient sparse file representation)\n");
	fprintf(stderr, "\t-f \t\tforce to do dangerous operation, make sure that you are aware of the dangers\n");
	fprintf(stderr, "\t-u \t\tchange fsid, use a random one\n");
	fprintf(stderr, "\t-U UUID\t\tchange fsid to UUID\n");
}

int main(int argc, char *argv[])
{
	struct btrfs_root *root;
	enum btrfs_open_ctree_flags ctree_flags = OPEN_CTREE_WRITES;
	int success = 0;
	int total = 0;
	int seeding_flag = 0;
	u64 seeding_value = 0;
	int random_fsid = 0;
	char *new_fsid_str = NULL;
	int ret;
	u64 super_flags = 0;

	optind = 1;
	while(1) {
		static const struct option long_options[] = {
			{ "help", no_argument, NULL, GETOPT_VAL_HELP},
			{ NULL, 0, NULL, 0 }
		};
		int c = getopt_long(argc, argv, "S:rxfuU:n", long_options, NULL);

		if (c < 0)
			break;
		switch(c) {
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
		case GETOPT_VAL_HELP:
		default:
			print_usage();
			return c != GETOPT_VAL_HELP;
		}
	}

	set_argv0(argv);
	argc = argc - optind;
	device = argv[optind];
	if (check_argc_exact(argc, 1)) {
		print_usage();
		return 1;
	}

	if (random_fsid && new_fsid_str) {
		fprintf(stderr,
			"ERROR: Random fsid can't be used with specified fsid\n");
		return 1;
	}
	if (!super_flags && !seeding_flag && !(random_fsid || new_fsid_str)) {
		fprintf(stderr,
			"ERROR: At least one option should be assigned.\n");
		print_usage();
		return 1;
	}

	if (new_fsid_str) {
		uuid_t tmp;

		ret = uuid_parse(new_fsid_str, tmp);
		if (ret < 0) {
			fprintf(stderr,
				"ERROR: Could not parse UUID: %s\n",
				new_fsid_str);
			return 1;
		}
		if (!test_uuid_unique(new_fsid_str)) {
			fprintf(stderr,
				"ERROR: Fsid %s is not unique\n",
				new_fsid_str);
			return 1;
		}
	}

	ret = check_mounted(device);
	if (ret < 0) {
		fprintf(stderr, "Could not check mount status: %s\n",
			strerror(-ret));
		return 1;
	} else if (ret) {
		fprintf(stderr, "%s is mounted\n", device);
		return 1;
	}

	root = open_ctree(device, 0, ctree_flags);

	if (!root) {
		fprintf(stderr, "Open ctree failed\n");
		return 1;
	}

	if (seeding_flag) {
		if (!seeding_value && !force) {
			fprintf(stderr, "Warning: This is dangerous, clearing the seeding flag may cause the derived device not to be mountable!\n");
			ret = ask_user("We are going to clear the seeding flag, are you sure?");
			if (!ret) {
				fprintf(stderr, "Clear seeding flag canceled\n");
				ret = 1;
				goto out;
			}
		}

		ret = update_seeding_flag(root, seeding_value);
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

	if (random_fsid || new_fsid_str) {
		if (!force) {
			fprintf(stderr,
				"Warning: It's highly recommended to run 'btrfs check' before this operation\n");
			fprintf(stderr,
				"Also canceling running UUID change progress may cause corruption\n");
			ret = ask_user("We are going to change UUID, are your sure?");
			if (!ret) {
				fprintf(stderr, "UUID change canceled\n");
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
		fprintf(stderr, "btrfstune failed\n");
	}
out:
	close_ctree(root);
	btrfs_close_all_devices();

	return ret;
}
