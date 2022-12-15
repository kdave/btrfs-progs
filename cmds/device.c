/*
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
#include <sys/ioctl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <errno.h>
#include <getopt.h>
#include <dirent.h>
#include <stdbool.h>
#include "kernel-shared/ctree.h"
#include "kernel-shared/zoned.h"
#include "common/string-table.h"
#include "common/utils.h"
#include "common/help.h"
#include "common/path-utils.h"
#include "common/device-utils.h"
#include "common/device-scan.h"
#include "common/format-output.h"
#include "common/open-utils.h"
#include "common/units.h"
#include "common/string-utils.h"
#include "common/messages.h"
#include "cmds/commands.h"
#include "cmds/filesystem-usage.h"
#include "mkfs/common.h"
#include "ioctl.h"

static const char * const device_cmd_group_usage[] = {
	"btrfs device <command> [<args>]",
	NULL
};

static const char * const cmd_device_add_usage[] = {
	"btrfs device add [options] <device> [<device>...] <path>",
	"Add one or more devices to a mounted filesystem.",
	"",
	"-K|--nodiscard    do not perform whole device TRIM on devices that report such capability",
	"-f|--force        force overwrite existing filesystem on the disk",
	"--enqueue         wait if there's another exclusive operation running,",
	"                  otherwise continue",
	NULL
};

static int cmd_device_add(const struct cmd_struct *cmd,
			  int argc, char **argv)
{
	char	*mntpnt;
	int i, fdmnt, ret = 0;
	DIR	*dirstream = NULL;
	bool discard = true;
	bool force = false;
	int last_dev;
	bool enqueue = false;
	int zoned;
	struct btrfs_ioctl_feature_flags feature_flags;

	optind = 0;
	while (1) {
		int c;
		enum { GETOPT_VAL_ENQUEUE = GETOPT_VAL_FIRST };
		static const struct option long_options[] = {
			{ "nodiscard", optional_argument, NULL, 'K'},
			{ "force", no_argument, NULL, 'f'},
			{ "enqueue", no_argument, NULL, GETOPT_VAL_ENQUEUE},
			{ NULL, 0, NULL, 0}
		};

		c = getopt_long(argc, argv, "Kf", long_options, NULL);
		if (c < 0)
			break;
		switch (c) {
		case 'K':
			discard = false;
			break;
		case 'f':
			force = true;
			break;
		case GETOPT_VAL_ENQUEUE:
			enqueue = true;
			break;
		default:
			usage_unknown_option(cmd, argv);
		}
	}

	if (check_argc_min(argc - optind, 2))
		return 1;

	last_dev = argc - 1;
	mntpnt = argv[last_dev];

	fdmnt = btrfs_open_dir(mntpnt, &dirstream, 1);
	if (fdmnt < 0)
		return 1;

	ret = check_running_fs_exclop(fdmnt, BTRFS_EXCLOP_DEV_ADD, enqueue);
	if (ret != 0) {
		if (ret < 0)
			error("unable to check status of exclusive operation: %m");
		close_file_or_dir(fdmnt, dirstream);
		return 1;
	}

	ret = ioctl(fdmnt, BTRFS_IOC_GET_FEATURES, &feature_flags);
	if (ret) {
		error("error getting feature flags '%s': %m", mntpnt);
		return 1;
	}
	zoned = (feature_flags.incompat_flags & BTRFS_FEATURE_INCOMPAT_ZONED);

	for (i = optind; i < last_dev; i++){
		struct btrfs_ioctl_vol_args ioctl_args;
		int	devfd, res;
		u64 dev_block_count = 0;
		char *path;

		if (!zoned && zoned_model(argv[i]) == ZONED_HOST_MANAGED) {
			error(
"zoned: cannot add host-managed zoned device to non-zoned filesystem '%s'",
			      argv[i]);
			ret++;
			continue;
		}

		res = test_dev_for_mkfs(argv[i], force);
		if (res) {
			ret++;
			continue;
		}

		devfd = open(argv[i], O_RDWR);
		if (devfd < 0) {
			error("unable to open device '%s'", argv[i]);
			ret++;
			continue;
		}

		res = btrfs_prepare_device(devfd, argv[i], &dev_block_count, 0,
				PREP_DEVICE_ZERO_END | PREP_DEVICE_VERBOSE |
				(discard ? PREP_DEVICE_DISCARD : 0) |
				(zoned ? PREP_DEVICE_ZONED : 0));
		close(devfd);
		if (res) {
			ret++;
			goto error_out;
		}

		path = path_canonicalize(argv[i]);
		if (!path) {
			error("could not canonicalize pathname '%s': %m",
				argv[i]);
			ret++;
			goto error_out;
		}

		memset(&ioctl_args, 0, sizeof(ioctl_args));
		strncpy_null(ioctl_args.name, path);
		res = ioctl(fdmnt, BTRFS_IOC_ADD_DEV, &ioctl_args);
		if (res < 0) {
			error("error adding device '%s': %m", path);
			ret++;
		}
		free(path);
	}

error_out:
	btrfs_warn_multiple_profiles(fdmnt);
	close_file_or_dir(fdmnt, dirstream);
	return !!ret;
}
static DEFINE_SIMPLE_COMMAND(device_add, "add");

static int _cmd_device_remove(const struct cmd_struct *cmd,
			      int argc, char **argv)
{
	char	*mntpnt;
	int i, fdmnt, ret = 0;
	DIR	*dirstream = NULL;
	bool enqueue = false;
	bool cancel = false;

	optind = 0;
	while (1) {
		int c;
		enum { GETOPT_VAL_ENQUEUE = GETOPT_VAL_FIRST };
		static const struct option long_options[] = {
			{ "enqueue", no_argument, NULL, GETOPT_VAL_ENQUEUE},
			{ NULL, 0, NULL, 0}
		};

		c = getopt_long(argc, argv, "", long_options, NULL);
		if (c < 0)
			break;
		switch (c) {
		case GETOPT_VAL_ENQUEUE:
			enqueue = true;
			break;
		default:
			usage_unknown_option(cmd, argv);
		}
	}

	if (check_argc_min(argc - optind, 2))
		return 1;

	mntpnt = argv[argc - 1];

	fdmnt = btrfs_open_dir(mntpnt, &dirstream, 1);
	if (fdmnt < 0)
		return 1;

	/* Scan device arguments for 'cancel', that must be the only "device" */
	for (i = optind; i < argc - 1; i++) {
		if (cancel) {
			error("cancel requested but another device specified: %s\n",
				argv[i]);
			close_file_or_dir(fdmnt, dirstream);
			return 1;
		}
		if (strcmp("cancel", argv[i]) == 0) {
			cancel = true;
			pr_verbose(LOG_DEFAULT, "Request to cancel running device deletion\n");
		}
	}

	if (!cancel) {
		ret = check_running_fs_exclop(fdmnt, BTRFS_EXCLOP_DEV_REMOVE,
					      enqueue);
		if (ret != 0) {
			if (ret < 0)
				error(
			"unable to check status of exclusive operation: %m");
			close_file_or_dir(fdmnt, dirstream);
			return 1;
		}
	}

	for(i = optind; i < argc - 1; i++) {
		struct	btrfs_ioctl_vol_args arg;
		struct btrfs_ioctl_vol_args_v2 argv2 = {0};
		bool is_devid = false;
		int	res;

		if (string_is_numerical(argv[i])) {
			argv2.devid = arg_strtou64(argv[i]);
			argv2.flags = BTRFS_DEVICE_SPEC_BY_ID;
			is_devid = true;
		} else if (strcmp(argv[i], "missing") == 0 ||
			   cancel ||
			   path_is_block_device(argv[i]) == 1) {
			strncpy_null(argv2.name, argv[i]);
		} else {
			error("not a block device: %s", argv[i]);
			ret++;
			continue;
		}

		/*
		 * Positive values are from BTRFS_ERROR_DEV_*,
		 * otherwise it's a generic error, one of errnos
		 */
		res = ioctl(fdmnt, BTRFS_IOC_RM_DEV_V2, &argv2);

		/*
		 * If BTRFS_IOC_RM_DEV_V2 is not supported we get ENOTTY and if
		 * argv2.flags includes a flag which kernel doesn't understand then
		 * we shall get EOPNOTSUPP
		 */
		if (res < 0 && (errno == ENOTTY || errno == EOPNOTSUPP)) {
			if (is_devid) {
				error("device delete by id failed: %m");
				ret++;
				continue;
			}
			memset(&arg, 0, sizeof(arg));
			strncpy_null(arg.name, argv[i]);
			res = ioctl(fdmnt, BTRFS_IOC_RM_DEV, &arg);
		}

		if (res) {
			const char *msg;

			if (res > 0)
				msg = btrfs_err_str(res);
			else
				msg = strerror(errno);
			if (is_devid) {
				error("error removing devid %llu: %s",
					argv2.devid, msg);
			} else {
				error("error removing device '%s': %s",
					argv[i], msg);
			}
			ret++;
		}
	}

	btrfs_warn_multiple_profiles(fdmnt);
	close_file_or_dir(fdmnt, dirstream);
	return !!ret;
}

#define COMMON_USAGE_REMOVE_DELETE					\
	"Remove a device from a filesystem, specified by a path to the device or", \
	"as a device id in the filesystem. The btrfs signature is removed from",   \
	"the device.",								\
	"If 'missing' is specified for <device>, the first device that is",	\
	"described by the filesystem metadata, but not present at the mount",	\
	"time will be removed. (only in degraded mode)",			\
	"If 'cancel' is specified as the only device to delete, request cancellation", \
	"of a previously started device deletion and wait until kernel finishes", \
	"any pending work. This will not delete the device and the size will be", \
	"restored to previous state. When deletion is not running, this will fail."

static const char * const cmd_device_remove_usage[] = {
	"btrfs device remove <device>|<devid> [<device>|<devid>...] <path>",
	"Remove a device from a filesystem",
	COMMON_USAGE_REMOVE_DELETE,
	"",
	"--enqueue         wait if there's another exclusive operation running,",
	"                  otherwise continue",
	NULL
};

static int cmd_device_remove(const struct cmd_struct *cmd,
			     int argc, char **argv)
{
	return _cmd_device_remove(cmd, argc, argv);
}
static DEFINE_SIMPLE_COMMAND(device_remove, "remove");

static const char * const cmd_device_delete_usage[] = {
	"btrfs device delete <device>|<devid> [<device>|<devid>...] <path>",
	"Remove a device from a filesystem (alias of \"btrfs device remove\")",
	COMMON_USAGE_REMOVE_DELETE,
	"",
	"--enqueue         wait if there's another exclusive operation running,",
	"                  otherwise continue",
	NULL
};

static int cmd_device_delete(const struct cmd_struct *cmd,
			     int argc, char **argv)
{
	return _cmd_device_remove(cmd, argc, argv);
}
static DEFINE_COMMAND(device_delete, "delete", cmd_device_delete,
		      cmd_device_delete_usage, NULL, CMD_ALIAS);

static int btrfs_forget_devices(const char *path)
{
	struct btrfs_ioctl_vol_args args;
	int ret;
	int fd;

	fd = open("/dev/btrfs-control", O_RDWR);
	if (fd < 0)
		return -errno;

	memset(&args, 0, sizeof(args));
	if (path)
		strncpy_null(args.name, path);
	ret = ioctl(fd, BTRFS_IOC_FORGET_DEV, &args);
	if (ret)
		ret = -errno;
	close(fd);
	return ret;
}

static const char * const cmd_device_scan_usage[] = {
	"btrfs device scan [-d|--all-devices] <device> [<device>...]\n"
	"btrfs device scan -u|--forget [<device>...]",
	"Scan or forget (unregister) devices of btrfs filesystems",
	"Scan or forget (unregister) devices of btrfs filesystems. Multi-device",
	"filesystems need to scan devices before mount. The blkid provides list",
	"of devices in case no path is given. If blkid is no available, there's",
	"a fallback to manual enumeration of device nodes.",
	"",
	"The reverse is done by the forget option, such devices must be unmounted.",
	"No argument will unregister all devices that are not part of a mounted filesystem.",
	"",
	" -d|--all-devices            enumerate and register all devices, use as a fallback",
	"                             if blkid is not available",
	" -u|--forget [<device>...]   unregister a given device or all stale devices if no path ",
	HELPINFO_INSERT_GLOBALS,
	HELPINFO_INSERT_VERBOSE,
	HELPINFO_INSERT_QUIET,
	NULL
};

static int cmd_device_scan(const struct cmd_struct *cmd, int argc, char **argv)
{
	int i;
	int devstart;
	bool all = false;
	bool forget = 0;
	int ret = 0;

	optind = 0;
	while (1) {
		int c;
		static const struct option long_options[] = {
			{ "all-devices", no_argument, NULL, 'd'},
			{ "forget", no_argument, NULL, 'u'},
			{ NULL, 0, NULL, 0}
		};

		c = getopt_long(argc, argv, "du", long_options, NULL);
		if (c < 0)
			break;
		switch (c) {
		case 'd':
			all = true;
			break;
		case 'u':
			forget = true;
			break;
		default:
			usage_unknown_option(cmd, argv);
		}
	}
	devstart = optind;

	if (all && forget)
		usage(cmd);

	if (all && check_argc_max(argc - optind, 1))
		usage(cmd);

	if (all || argc - optind == 0) {
		if (forget) {
			ret = btrfs_forget_devices(NULL);
			if (ret < 0) {
				errno = -ret;
				error("cannot unregister devices: %m");
			}
		} else {
			pr_verbose(LOG_DEFAULT, "Scanning for Btrfs filesystems\n");
			ret = btrfs_scan_devices(1);
			error_on(ret, "error %d while scanning", ret);
			ret = btrfs_register_all_devices();
			error_on(ret,
				"there were %d errors while registering devices",
				ret);
		}
		goto out;
	}

	for( i = devstart ; i < argc ; i++ ){
		char *path;

		if (path_is_block_device(argv[i]) != 1) {
			error("not a block device: %s", argv[i]);
			ret = 1;
			goto out;
		}
		path = path_canonicalize(argv[i]);
		if (!path) {
			error("could not canonicalize path '%s': %m", argv[i]);
			ret = 1;
			goto out;
		}
		if (forget) {
			ret = btrfs_forget_devices(path);
			if (ret < 0) {
				errno = -ret;
				error("cannot unregister device '%s': %m", path);
			}
		} else {
			pr_verbose(LOG_DEFAULT, "Scanning for btrfs filesystems on '%s'\n", path);
			if (btrfs_register_one_device(path) != 0) {
				ret = 1;
				free(path);
				goto out;
			}
		}
		free(path);
	}

out:
	return !!ret;
}
static DEFINE_SIMPLE_COMMAND(device_scan, "scan");

static const char * const cmd_device_ready_usage[] = {
	"btrfs device ready <device>",
	"Check and wait until a group of devices of a filesystem is ready for mount",
	NULL
};

static int cmd_device_ready(const struct cmd_struct *cmd, int argc, char **argv)
{
	struct	btrfs_ioctl_vol_args args;
	int	fd;
	int	ret;
	char	*path;

	clean_args_no_options(cmd, argc, argv);

	if (check_argc_exact(argc - optind, 1))
		return 1;

	fd = open("/dev/btrfs-control", O_RDWR);
	if (fd < 0) {
		perror("failed to open /dev/btrfs-control");
		return 1;
	}

	path = path_canonicalize(argv[optind]);
	if (!path) {
		error("could not canonicalize pathname '%s': %m",
			argv[optind]);
		ret = 1;
		goto out;
	}

	if (path_is_block_device(path) != 1) {
		error("not a block device: %s", path);
		ret = 1;
		goto out;
	}

	memset(&args, 0, sizeof(args));
	strncpy_null(args.name, path);
	ret = ioctl(fd, BTRFS_IOC_DEVICES_READY, &args);
	if (ret < 0) {
		error("unable to determine if device '%s' is ready for mount: %m",
			path);
		ret = 1;
	}

out:
	free(path);
	close(fd);
	return ret;
}
static DEFINE_SIMPLE_COMMAND(device_ready, "ready");

static const struct rowspec device_stats_rowspec[] = {
	{ .key = "device", .fmt = "str", .out_text = "device", .out_json = "device" },
	{ .key = "devid", .fmt = "%llu", .out_text = "devid", .out_json = "devid" },
	{ .key = "write_io_errs", .fmt = "%llu", .out_text = "write_io_errs", .out_json = "write_io_errs" },
	{ .key = "read_io_errs", .fmt = "%llu", .out_text = "read_io_errs", .out_json = "read_io_errs" },
	{ .key = "flush_io_errs", .fmt = "%llu", .out_text = "flush_io_errs", .out_json = "flush_io_errs" },
	{ .key = "corruption_errs", .fmt = "%llu", .out_text = "corruption_errs", .out_json = "corruption_errs" },
	{ .key = "generation_errs", .fmt = "%llu", .out_text = "generation_errs", .out_json = "generation_errs" },
	ROWSPEC_END
};

static const char * const cmd_device_stats_usage[] = {
	"btrfs device stats [options] <path>|<device>",
	"Show device IO error statistics",
	"Show device IO error statistics for all devices of the given filesystem",
	"identified by PATH or DEVICE. The filesystem must be mounted.",
	"",
	"-c|--check             return non-zero if any stat counter is not zero",
	"-z|--reset             show current stats and reset values to zero",
	"-T                     show current stats in tabular format",
	HELPINFO_INSERT_GLOBALS,
	HELPINFO_INSERT_FORMAT,
	NULL
};

static int print_device_stat_string(struct format_ctx *fctx,
		struct btrfs_ioctl_get_dev_stats *args, char *path, bool check)
{
	char *canonical_path = path_canonicalize(path);
	char devid_str[32];
	int j;
	int err = 0;
	static const struct {
		const char name[32];
		enum btrfs_dev_stat_values stat_idx;
	} dev_stats[] = {
		{ "write_io_errs", BTRFS_DEV_STAT_WRITE_ERRS },
		{ "read_io_errs", BTRFS_DEV_STAT_READ_ERRS },
		{ "flush_io_errs", BTRFS_DEV_STAT_FLUSH_ERRS },
		{ "corruption_errs", BTRFS_DEV_STAT_CORRUPTION_ERRS },
		{ "generation_errs", BTRFS_DEV_STAT_GENERATION_ERRS },
	};
	/*
	 * The plain text and json formats cannot be mapped directly in all
	 * cases and we have to switch.
	 */
	const bool json = (bconf.output_format == CMD_FORMAT_JSON);

	/* No path when device is missing. */
	if (!canonical_path) {
		canonical_path = malloc(32);

		if (!canonical_path) {
			error_msg(ERROR_MSG_MEMORY, "device path buffer");
			return -ENOMEM;
		}

		snprintf(canonical_path, 32, "devid:%llu", args->devid);
	}
	snprintf(devid_str, 32, "%llu", args->devid);
	fmt_print_start_group(fctx, NULL, JSON_TYPE_MAP);
	/* Plain text does not print device info */
	if (json) {
		fmt_print(fctx, "device", canonical_path);
		fmt_print(fctx, "devid", args->devid);
	}

	for (j = 0; j < ARRAY_SIZE(dev_stats); j++) {
		enum btrfs_dev_stat_values stat_idx = dev_stats[j].stat_idx;

		/* We got fewer items than we know */
		if (args->nr_items < stat_idx + 1)
			continue;

		/* Own format due to [/dev/name].value */
		if (json) {
			fmt_print(fctx, dev_stats[j].name, args->values[stat_idx]);
		} else {
			pr_verbose(LOG_DEFAULT, "[%s].%-16s %llu\n", canonical_path, dev_stats[j].name,
					args->values[stat_idx]);
		}
		if (check && (args->values[stat_idx] > 0))
			err |= 64;
	}

	fmt_print_end_group(fctx, NULL);
	free(canonical_path);

	return err;
}


static int print_device_stat_tabular(struct string_table *table, int row,
		struct btrfs_ioctl_get_dev_stats *args, char *path, bool check)
{
	char *canonical_path = path_canonicalize(path);
	int j;
	int err = 0;
	static const struct {
		const char name[32];
		enum btrfs_dev_stat_values stat_idx;
	} dev_stats[] = {
		{ "write_io_errs", BTRFS_DEV_STAT_WRITE_ERRS },
		{ "read_io_errs", BTRFS_DEV_STAT_READ_ERRS },
		{ "flush_io_errs", BTRFS_DEV_STAT_FLUSH_ERRS },
		{ "corruption_errs", BTRFS_DEV_STAT_CORRUPTION_ERRS },
		{ "generation_errs", BTRFS_DEV_STAT_GENERATION_ERRS },
	};

	/* Skip header + --- line */
	row += 2;

	/* No path when device is missing. */
	if (!canonical_path) {
		canonical_path = malloc(32);

		if (!canonical_path) {
			error_msg(ERROR_MSG_MEMORY, "device path buffer");
			return -ENOMEM;
		}

		snprintf(canonical_path, 32, "devid:%llu", args->devid);
	}
	table_printf(table, 0, row, ">%llu", args->devid);
	table_printf(table, 1, row, ">%s", canonical_path);
	free(canonical_path);

	for (j = 0; j < ARRAY_SIZE(dev_stats); j++) {
		enum btrfs_dev_stat_values stat_idx = dev_stats[j].stat_idx;

		/* We got fewer items than we know */
		if (args->nr_items < stat_idx + 1)
			continue;

		table_printf(table, 2, row, ">%llu", args->values[stat_idx]);
		table_printf(table, 3, row, ">%llu", args->values[stat_idx]);
		table_printf(table, 4, row, ">%llu", args->values[stat_idx]);
		table_printf(table, 5, row, ">%llu", args->values[stat_idx]);
		table_printf(table, 6, row, ">%llu", args->values[stat_idx]);

		if (check && (args->values[stat_idx] > 0))
			err |= 64;
	}

	return err;
}

static int cmd_device_stats(const struct cmd_struct *cmd, int argc, char **argv)
{
	char *dev_path;
	struct btrfs_ioctl_fs_info_args fi_args;
	struct btrfs_ioctl_dev_info_args *di_args = NULL;
	struct string_table *table = NULL;
	int ret;
	int fdmnt;
	int i;
	int err = 0;
	bool check = false;
	bool free_table = false;
	bool tabular = false;
	__u64 flags = 0;
	DIR *dirstream = NULL;
	struct format_ctx fctx;

	optind = 0;
	while (1) {
		int c;
		static const struct option long_options[] = {
			{"check", no_argument, NULL, 'c'},
			{"reset", no_argument, NULL, 'z'},
			{NULL, 0, NULL, 0}
		};

		c = getopt_long(argc, argv, "czT", long_options, NULL);
		if (c < 0)
			break;

		switch (c) {
		case 'c':
			check = true;
			break;
		case 'z':
			flags = BTRFS_DEV_STATS_RESET;
			break;
		case 'T':
			tabular = true;
			break;
		default:
			usage_unknown_option(cmd, argv);
		}
	}

	if (check_argc_exact(argc - optind, 1))
		return 1;

	dev_path = argv[optind];

	fdmnt = open_path_or_dev_mnt(dev_path, &dirstream, 1);
	if (fdmnt < 0)
		return 1;

	ret = get_fs_info(dev_path, &fi_args, &di_args);
	if (ret) {
		errno = -ret;
		error("getting device info for %s failed: %m", dev_path);
		err = 1;
		goto out;
	}
	if (!fi_args.num_devices) {
		error("no devices found");
		err = 1;
		goto out;
	}

	if (tabular) {
		/*
		 * cols = Id/Path/write/read/flush/corruption/generation
		 * rows = num devices + 2 (header and ---- line)
		 */
		table = table_create(7, fi_args.num_devices + 2);
		if (!table) {
			error_msg(ERROR_MSG_MEMORY, NULL);
			goto out;
		}
		free_table = true;
		table_printf(table, 0,0, "<Id");
		table_printf(table, 1,0, "<Path");
		table_printf(table, 2,0, "<Write errors");
		table_printf(table, 3,0, "<Read errors");
		table_printf(table, 4,0, "<Flush errors");
		table_printf(table, 5,0, "<Corruption errors");
		table_printf(table, 6,0, "<Generation errors");
		for (i = 0; i < 7; i++)
			table_printf(table, i, 1, "*-");
	} else {
		fmt_start(&fctx, device_stats_rowspec, 24, 0);
		fmt_print_start_group(&fctx, "device-stats", JSON_TYPE_ARRAY);
	}

	for (i = 0; i < fi_args.num_devices; i++) {
		struct btrfs_ioctl_get_dev_stats args = {0};
		char path[BTRFS_DEVICE_PATH_NAME_MAX + 1];
		int err2;

		strncpy(path, (char *)di_args[i].path,
			BTRFS_DEVICE_PATH_NAME_MAX);
		path[BTRFS_DEVICE_PATH_NAME_MAX] = 0;

		args.devid = di_args[i].devid;
		args.nr_items = BTRFS_DEV_STAT_VALUES_MAX;
		args.flags = flags;

		if (ioctl(fdmnt, BTRFS_IOC_GET_DEV_STATS, &args) < 0) {
			error("device stats ioctl failed on %s: %m",
			      path);
			err |= 1;
			goto out;
		}

		if (tabular)
			err2 = print_device_stat_tabular(table, i, &args, path, check);
		else
			err2 = print_device_stat_string(&fctx, &args, path, check);

		if (err2) {
			if (err2 < 0) {
				err = err2;
				goto out;
			} else {
				err |= err2;
			}
		}
	}

	if (tabular) {
		table_dump(table);
	} else {
		fmt_print_end_group(&fctx, "device-stats");
		fmt_end(&fctx);
	}

out:
	free(di_args);
	close_file_or_dir(fdmnt, dirstream);
	if (free_table)
		table_free(table);

	return err;
}
static DEFINE_COMMAND_WITH_FLAGS(device_stats, "stats", CMD_FORMAT_JSON);

static const char * const cmd_device_usage_usage[] = {
	"btrfs device usage [options] <path> [<path>..]",
	"Show detailed information about internal allocations in devices.",
	"",
	HELPINFO_UNITS_SHORT_LONG,
	NULL
};

static int _cmd_device_usage(int fd, const char *path, unsigned unit_mode)
{
	int i;
	int ret = 0;
	struct chunk_info *chunkinfo = NULL;
	struct device_info *devinfo = NULL;
	int chunkcount = 0;
	int devcount = 0;

	ret = load_chunk_and_device_info(fd, &chunkinfo, &chunkcount, &devinfo,
			&devcount);
	if (ret)
		goto out;

	for (i = 0; i < devcount; i++) {
		pr_verbose(LOG_DEFAULT, "%s, ID: %llu\n", devinfo[i].path, devinfo[i].devid);
		print_device_sizes(&devinfo[i], unit_mode);
		print_device_chunks(&devinfo[i], chunkinfo, chunkcount,
				unit_mode);
		pr_verbose(LOG_DEFAULT, "\n");
	}

out:
	free(devinfo);
	free(chunkinfo);

	return ret;
}

static int cmd_device_usage(const struct cmd_struct *cmd, int argc, char **argv)
{
	unsigned unit_mode;
	int ret = 0;
	int i;

	unit_mode = get_unit_mode_from_arg(&argc, argv, 1);

	clean_args_no_options(cmd, argc, argv);

	if (check_argc_min(argc - optind, 1))
		return 1;

	for (i = optind; i < argc; i++) {
		int fd;
		DIR *dirstream = NULL;

		if (i > 1)
			pr_verbose(LOG_DEFAULT, "\n");

		fd = btrfs_open_dir(argv[i], &dirstream, 1);
		if (fd < 0) {
			ret = 1;
			break;
		}

		ret = _cmd_device_usage(fd, argv[i], unit_mode);
		btrfs_warn_multiple_profiles(fd);
		close_file_or_dir(fd, dirstream);

		if (ret)
			break;
	}

	return !!ret;
}
static DEFINE_SIMPLE_COMMAND(device_usage, "usage");

static const char * const cmd_device_replace_usage[] = {
	"btrfs device replace <command> [...]\n"
	"\tReplace a device (alias of \"btrfs replace\")",
	"Please see \"btrfs replace --help\" for more information.",
	NULL
};

static int cmd_device_replace(const struct cmd_struct *unused,
			      int argc, char **argv)
{
	return cmd_execute(&cmd_struct_replace, argc, argv);
}

/* Alias of 1st level command 'replace' as a subcommand of 'device' */
static DEFINE_COMMAND(device_replace, "replace", cmd_device_replace,
		      cmd_device_replace_usage, NULL, CMD_ALIAS);

static const char device_cmd_group_info[] =
"manage and query devices in the filesystem";

static const struct cmd_group device_cmd_group = {
	device_cmd_group_usage, device_cmd_group_info, {
		&cmd_struct_device_add,
		&cmd_struct_device_delete,
		&cmd_struct_device_remove,
		&cmd_struct_device_replace,
		&cmd_struct_device_scan,
		&cmd_struct_device_ready,
		&cmd_struct_device_stats,
		&cmd_struct_device_usage,
		NULL
	}
};

DEFINE_GROUP_COMMAND_TOKEN(device);
