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
#include <stdlib.h>
#include <unistd.h>
#include <getopt.h>
#include <errno.h>
#include <stdbool.h>
#include <string.h>
#include "kernel-shared/ctree.h"
#include "kernel-shared/disk-io.h"
#include "kernel-shared/volumes.h"
#include "kernel-shared/accessors.h"
#include "crypto/hash.h"
#include "common/internal.h"
#include "common/messages.h"
#include "common/cpu-utils.h"
#include "common/box.h"
#include "common/utils.h"
#include "common/help.h"
#include "common/open-utils.h"
#include "common/string-utils.h"
#include "cmds/commands.h"
#include "image/metadump.h"
#include "image/sanitize.h"
#include "image/common.h"

static const char * const image_usage[] = {
	"btrfs-image [options] source target",
	"Create or restore a filesystem image (metadata)",
	"",
	"Options:",
	OPTLINE("-r", "restore metadump image"),
	OPTLINE("-c value", "compression level (0 ~ 9)"),
	OPTLINE("-t value", "number of threads (1 ~ 32)"),
	OPTLINE("-o", "don't mess with the chunk tree when restoring"),
	OPTLINE("-s", "sanitize file names, use once to just use garbage, use twice if you want crc collisions"),
	OPTLINE("-w", "walk all trees instead of using extent tree, do this if your extent tree is broken"),
	OPTLINE("-m", "restore for multiple devices"),
	OPTLINE("-d", "also dump data, conflicts with -w"),
	"",
	"General:",
	OPTLINE("--version", "print the btrfs-image version, builtin featurues and exit"),
	OPTLINE("--help", "print this help and exit"),
	"",
	"In the dump mode, source is the btrfs device and target is the output file (use '-' for stdout).",
	"In the restore mode, source is the dumped image and target is the btrfs device/file.",
	NULL
};

static const struct cmd_struct image_cmd = {
	.usagestr = image_usage
};

int BOX_MAIN(image)(int argc, char *argv[])
{
	char *source;
	char *target;
	u64 num_threads = 0;
	u64 compress_level = 0;
	int create = 1;
	int old_restore = 0;
	int walk_trees = 0;
	int multi_devices = 0;
	int ret;
	enum sanitize_mode sanitize = SANITIZE_NONE;
	int dev_cnt = 0;
	bool dump_data = false;
	int usage_error = 0;
	FILE *out;

	cpu_detect_flags();
	hash_init_accel();

	while (1) {
		enum { GETOPT_VAL_VERSION = GETOPT_VAL_FIRST };
		static const struct option long_options[] = {
			{ "help", no_argument, NULL, GETOPT_VAL_HELP},
			{ "version", no_argument, NULL, GETOPT_VAL_VERSION },
			{ NULL, 0, NULL, 0 }
		};
		int c = getopt_long(argc, argv, "rc:t:oswmd", long_options, NULL);
		if (c < 0)
			break;
		switch (c) {
		case 'r':
			create = 0;
			break;
		case 't':
			num_threads = arg_strtou64(optarg);
			if (num_threads > MAX_WORKER_THREADS) {
				error("number of threads out of range: %llu > %d",
					num_threads, MAX_WORKER_THREADS);
				return 1;
			}
			break;
		case 'c':
			compress_level = arg_strtou64(optarg);
			if (compress_level > 9) {
				error("compression level out of range: %llu",
					compress_level);
				return 1;
			}
			break;
		case 'o':
			old_restore = 1;
			break;
		case 's':
			if (sanitize == SANITIZE_NONE)
				sanitize = SANITIZE_NAMES;
			else if (sanitize == SANITIZE_NAMES)
				sanitize = SANITIZE_COLLISIONS;
			break;
		case 'w':
			walk_trees = 1;
			break;
		case 'm':
			create = 0;
			multi_devices = 1;
			break;
		case 'd':
			btrfs_warn_experimental("Feature: dump image with data");
			dump_data = true;
			break;
		case GETOPT_VAL_VERSION:
			help_builtin_features("btrfs-image, part of ");
			ret = 0;
			goto success;
		case GETOPT_VAL_HELP:
		default:
			usage(&image_cmd, c != GETOPT_VAL_HELP);
		}
	}

	set_argv0(argv);
	if (check_argc_min(argc - optind, 2))
		usage(&image_cmd, 1);

	dev_cnt = argc - optind - 1;

#if !EXPERIMENTAL
	if (dump_data) {
		error(
"data dump feature is experimental and is not configured in this build");
		usage(&image_cmd, 1);
	}
#endif
	if (create) {
		if (old_restore) {
			error(
			"create and restore cannot be used at the same time");
			usage_error++;
		}
		if (dump_data && walk_trees) {
			error("-d conflicts with -w option");
			usage_error++;
		}
	} else {
		if (walk_trees || sanitize != SANITIZE_NONE || compress_level ||
		    dump_data) {
			error(
		"using -w, -s, -c, -d options for restore makes no sense");
			usage_error++;
		}
		if (multi_devices && dev_cnt < 2) {
			error("not enough devices specified for -m option");
			usage_error++;
		}
		if (!multi_devices && dev_cnt != 1) {
			error("accepts only 1 device without -m option");
			usage_error++;
		}
	}

	if (usage_error)
		usage(&image_cmd, 1);

	source = argv[optind];
	target = argv[optind + 1];

	if (create && !strcmp(target, "-")) {
		out = stdout;
	} else {
		out = fopen(target, "w+");
		if (!out) {
			error("unable to create target file %s", target);
			exit(1);
		}
	}

	if (compress_level > 0 || create == 0) {
		if (num_threads == 0) {
			long tmp = sysconf(_SC_NPROCESSORS_ONLN);

			if (tmp <= 0)
				tmp = 1;
			tmp = min_t(long, tmp, MAX_WORKER_THREADS);
			num_threads = tmp;
		}
	} else {
		num_threads = 0;
	}

	if (create) {
		ret = check_mounted(source);
		if (ret < 0) {
			errno = -ret;
			warning("unable to check mount status of: %m");
		} else if (ret) {
			warning("%s already mounted, results may be inaccurate",
					source);
		}

		ret = create_metadump(source, out, num_threads,
				      compress_level, sanitize, walk_trees,
				      dump_data);
	} else {
		ret = restore_metadump(source, out, old_restore, num_threads,
				       0, target, multi_devices);
	}
	if (ret) {
		error("%s failed: %d", (create) ? "create" : "restore", ret);
		goto out;
	}

	 /* extended support for multiple devices */
	if (!create && multi_devices) {
		struct open_ctree_args oca = { 0 };
		struct btrfs_fs_info *info;
		u64 total_devs;
		int i;

		oca.filename = target;
		oca.flags = OPEN_CTREE_PARTIAL | OPEN_CTREE_RESTORE |
			OPEN_CTREE_SKIP_LEAF_ITEM_CHECKS;
		info = open_ctree_fs_info(&oca);
		if (!info) {
			error("open ctree failed at %s", target);
			return 1;
		}

		total_devs = btrfs_super_num_devices(info->super_copy);
		if (total_devs != dev_cnt) {
			error("it needs %llu devices but has only %d",
				total_devs, dev_cnt);
			close_ctree(info->chunk_root);
			goto out;
		}

		/* update super block on other disks */
		for (i = 2; i <= dev_cnt; i++) {
			ret = update_disk_super_on_device(info,
					argv[optind + i], (u64)i);
			if (ret) {
				error("update disk superblock failed devid %d: %d",
					i, ret);
				close_ctree(info->chunk_root);
				exit(1);
			}
		}

		close_ctree(info->chunk_root);

		/* fix metadata block to map correct chunk */
		ret = restore_metadump(source, out, 0, num_threads, 1,
				       target, 1);
		if (ret) {
			error("unable to fixup metadump: %d", ret);
			exit(1);
		}
	}
out:
	if (out == stdout) {
		fflush(out);
	} else {
		fclose(out);
		if (ret && create) {
			int unlink_ret;

			unlink_ret = unlink(target);
			if (unlink_ret)
				error("unlink output file %s failed: %m",
						target);
		}
	}

	btrfs_close_all_devices();

success:
	return !!ret;
}
