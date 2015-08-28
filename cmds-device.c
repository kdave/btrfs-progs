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

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <sys/ioctl.h>
#include <errno.h>
#include <sys/stat.h>
#include <getopt.h>

#include "kerncompat.h"
#include "ctree.h"
#include "ioctl.h"
#include "utils.h"
#include "volumes.h"
#include "cmds-fi-usage.h"

#include "commands.h"

static const char * const device_cmd_group_usage[] = {
	"btrfs device <command> [<args>]",
	NULL
};

static const char * const cmd_device_add_usage[] = {
	"btrfs device add [options] <device> [<device>...] <path>",
	"Add a device to a filesystem",
	"-K|--nodiscard    do not perform whole device TRIM",
	"-f|--force        force overwrite existing filesystem on the disk",
	NULL
};

static int cmd_device_add(int argc, char **argv)
{
	char	*mntpnt;
	int	i, fdmnt, ret=0, e;
	DIR	*dirstream = NULL;
	int discard = 1;
	int force = 0;

	while (1) {
		int c;
		static const struct option long_options[] = {
			{ "nodiscard", optional_argument, NULL, 'K'},
			{ "force", no_argument, NULL, 'f'},
			{ NULL, 0, NULL, 0}
		};

		c = getopt_long(argc, argv, "Kf", long_options, NULL);
		if (c < 0)
			break;
		switch (c) {
		case 'K':
			discard = 0;
			break;
		case 'f':
			force = 1;
			break;
		default:
			usage(cmd_device_add_usage);
		}
	}

	argc = argc - optind;

	if (check_argc_min(argc, 2))
		usage(cmd_device_add_usage);

	mntpnt = argv[optind + argc - 1];

	fdmnt = btrfs_open_dir(mntpnt, &dirstream, 1);
	if (fdmnt < 0)
		return 1;

	for (i = optind; i < optind + argc - 1; i++){
		struct btrfs_ioctl_vol_args ioctl_args;
		int	devfd, res;
		u64 dev_block_count = 0;
		int mixed = 0;
		char *path;

		res = test_dev_for_mkfs(argv[i], force);
		if (res) {
			ret++;
			continue;
		}

		devfd = open(argv[i], O_RDWR);
		if (devfd < 0) {
			fprintf(stderr, "ERROR: Unable to open device '%s'\n", argv[i]);
			ret++;
			continue;
		}

		res = btrfs_prepare_device(devfd, argv[i], 1, &dev_block_count,
					   0, &mixed, discard);
		close(devfd);
		if (res) {
			ret++;
			goto error_out;
		}

		path = canonicalize_path(argv[i]);
		if (!path) {
			fprintf(stderr,
				"ERROR: Could not canonicalize pathname '%s': %s\n",
				argv[i], strerror(errno));
			ret++;
			goto error_out;
		}

		memset(&ioctl_args, 0, sizeof(ioctl_args));
		strncpy_null(ioctl_args.name, path);
		res = ioctl(fdmnt, BTRFS_IOC_ADD_DEV, &ioctl_args);
		e = errno;
		if (res < 0) {
			fprintf(stderr, "ERROR: error adding the device '%s' - %s\n",
				path, strerror(e));
			ret++;
		}
		free(path);
	}

error_out:
	close_file_or_dir(fdmnt, dirstream);
	btrfs_close_all_devices();
	return !!ret;
}

static int _cmd_device_remove(int argc, char **argv,
		const char * const *usagestr)
{
	char	*mntpnt;
	int	i, fdmnt, ret=0, e;
	DIR	*dirstream = NULL;

	if (check_argc_min(argc, 3))
		usage(usagestr);

	mntpnt = argv[argc - 1];

	fdmnt = btrfs_open_dir(mntpnt, &dirstream, 1);
	if (fdmnt < 0)
		return 1;

	for(i=1 ; i < argc - 1; i++ ){
		struct	btrfs_ioctl_vol_args arg;
		int	res;

		if (is_block_device(argv[i]) != 1) {
			fprintf(stderr,
				"ERROR: %s is not a block device\n", argv[i]);
			ret++;
			continue;
		}
		memset(&arg, 0, sizeof(arg));
		strncpy_null(arg.name, argv[i]);
		res = ioctl(fdmnt, BTRFS_IOC_RM_DEV, &arg);
		e = errno;
		if (res) {
			const char *msg;

			if (res > 0)
				msg = btrfs_err_str(res);
			else
				msg = strerror(e);
			fprintf(stderr,
				"ERROR: error removing the device '%s' - %s\n",
				argv[i], msg);
			ret++;
		}
	}

	close_file_or_dir(fdmnt, dirstream);
	return !!ret;
}

static const char * const cmd_device_remove_usage[] = {
	"btrfs device remove <device> [<device>...] <path>",
	"Remove a device from a filesystem",
	NULL
};

static int cmd_device_remove(int argc, char **argv)
{
	return _cmd_device_remove(argc, argv, cmd_device_remove_usage);
}

static const char * const cmd_device_delete_usage[] = {
	"btrfs device delete <device> [<device>...] <path>",
	"Remove a device from a filesystem",
	NULL
};

static int cmd_device_delete(int argc, char **argv)
{
	return _cmd_device_remove(argc, argv, cmd_device_delete_usage);
}

static const char * const cmd_device_scan_usage[] = {
	"btrfs device scan [(-d|--all-devices)|<device> [<device>...]]",
	"Scan devices for a btrfs filesystem",
	" -d|--all-devices (deprecated)",
	NULL
};

static int cmd_device_scan(int argc, char **argv)
{
	int i;
	int devstart = 1;
	int all = 0;
	int ret = 0;

	optind = 1;
	while (1) {
		int c;
		static const struct option long_options[] = {
			{ "all-devices", no_argument, NULL, 'd'},
			{ NULL, 0, NULL, 0}
		};

		c = getopt_long(argc, argv, "d", long_options, NULL);
		if (c < 0)
			break;
		switch (c) {
		case 'd':
			all = 1;
			break;
		default:
			usage(cmd_device_scan_usage);
		}
	}

	if (all && check_argc_max(argc, 2))
		usage(cmd_device_scan_usage);

	if (all || argc == 1) {
		printf("Scanning for Btrfs filesystems\n");
		ret = btrfs_scan_lblkid();
		if (ret)
			fprintf(stderr, "ERROR: error %d while scanning\n", ret);
		ret = btrfs_register_all_devices();
		if (ret)
			fprintf(stderr, "ERROR: error %d while registering\n", ret);
		goto out;
	}

	for( i = devstart ; i < argc ; i++ ){
		char *path;

		if (is_block_device(argv[i]) != 1) {
			fprintf(stderr,
				"ERROR: %s is not a block device\n", argv[i]);
			ret = 1;
			goto out;
		}
		path = canonicalize_path(argv[i]);
		if (!path) {
			fprintf(stderr,
				"ERROR: Could not canonicalize path '%s': %s\n",
				argv[i], strerror(errno));
			ret = 1;
			goto out;
		}
		printf("Scanning for Btrfs filesystems in '%s'\n", path);
		if (btrfs_register_one_device(path) != 0) {
			ret = 1;
			free(path);
			goto out;
		}
		free(path);
	}

out:
	btrfs_close_all_devices();
	return !!ret;
}

static const char * const cmd_device_ready_usage[] = {
	"btrfs device ready <device>",
	"Check device to see if it has all of its devices in cache for mounting",
	NULL
};

static int cmd_device_ready(int argc, char **argv)
{
	struct	btrfs_ioctl_vol_args args;
	int	fd;
	int	ret;
	char	*path;

	if (check_argc_min(argc, 2))
		usage(cmd_device_ready_usage);

	fd = open("/dev/btrfs-control", O_RDWR);
	if (fd < 0) {
		perror("failed to open /dev/btrfs-control");
		return 1;
	}

	path = canonicalize_path(argv[argc - 1]);
	if (!path) {
		fprintf(stderr,
			"ERROR: Could not canonicalize pathname '%s': %s\n",
			argv[argc - 1], strerror(errno));
		ret = 1;
		goto out;
	}

	if (is_block_device(path) != 1) {
		fprintf(stderr,
			"ERROR: %s is not a block device\n", path);
		ret = 1;
		goto out;
	}

	memset(&args, 0, sizeof(args));
	strncpy_null(args.name, path);
	ret = ioctl(fd, BTRFS_IOC_DEVICES_READY, &args);
	if (ret < 0) {
		fprintf(stderr, "ERROR: unable to determine if the device '%s'"
			" is ready for mounting - %s\n", path,
			strerror(errno));
		ret = 1;
	}

out:
	free(path);
	close(fd);
	return ret;
}

static const char * const cmd_device_stats_usage[] = {
	"btrfs device stats [-z] <path>|<device>",
	"Show current device IO stats.",
	"",
	"-z                     show current stats and reset values to zero",
	NULL
};

static int cmd_device_stats(int argc, char **argv)
{
	char *dev_path;
	struct btrfs_ioctl_fs_info_args fi_args;
	struct btrfs_ioctl_dev_info_args *di_args = NULL;
	int ret;
	int fdmnt;
	int i;
	int c;
	int err = 0;
	__u64 flags = 0;
	DIR *dirstream = NULL;

	optind = 1;
	while ((c = getopt(argc, argv, "z")) != -1) {
		switch (c) {
		case 'z':
			flags = BTRFS_DEV_STATS_RESET;
			break;
		case '?':
		default:
			usage(cmd_device_stats_usage);
		}
	}

	argc = argc - optind;
	if (check_argc_exact(argc, 1))
		usage(cmd_device_stats_usage);

	dev_path = argv[optind];

	fdmnt = open_path_or_dev_mnt(dev_path, &dirstream);

	if (fdmnt < 0) {
		if (errno == EINVAL)
			fprintf(stderr,
				"ERROR: '%s' is not a mounted btrfs device\n",
				dev_path);
		else
			fprintf(stderr, "ERROR: can't access '%s': %s\n",
				dev_path, strerror(errno));
		return 1;
	}

	ret = get_fs_info(dev_path, &fi_args, &di_args);
	if (ret) {
		fprintf(stderr, "ERROR: getting dev info for devstats failed: "
				"%s\n", strerror(-ret));
		err = 1;
		goto out;
	}
	if (!fi_args.num_devices) {
		fprintf(stderr, "ERROR: no devices found\n");
		err = 1;
		goto out;
	}

	for (i = 0; i < fi_args.num_devices; i++) {
		struct btrfs_ioctl_get_dev_stats args = {0};
		__u8 path[BTRFS_DEVICE_PATH_NAME_MAX + 1];

		strncpy((char *)path, (char *)di_args[i].path,
			BTRFS_DEVICE_PATH_NAME_MAX);
		path[BTRFS_DEVICE_PATH_NAME_MAX] = '\0';

		args.devid = di_args[i].devid;
		args.nr_items = BTRFS_DEV_STAT_VALUES_MAX;
		args.flags = flags;

		if (ioctl(fdmnt, BTRFS_IOC_GET_DEV_STATS, &args) < 0) {
			fprintf(stderr,
				"ERROR: ioctl(BTRFS_IOC_GET_DEV_STATS) on %s failed: %s\n",
				path, strerror(errno));
			err = 1;
		} else {
			char *canonical_path;

			canonical_path = canonicalize_path((char *)path);

			if (args.nr_items >= BTRFS_DEV_STAT_WRITE_ERRS + 1)
				printf("[%s].write_io_errs   %llu\n",
				       canonical_path,
				       (unsigned long long) args.values[
					BTRFS_DEV_STAT_WRITE_ERRS]);
			if (args.nr_items >= BTRFS_DEV_STAT_READ_ERRS + 1)
				printf("[%s].read_io_errs    %llu\n",
				       canonical_path,
				       (unsigned long long) args.values[
					BTRFS_DEV_STAT_READ_ERRS]);
			if (args.nr_items >= BTRFS_DEV_STAT_FLUSH_ERRS + 1)
				printf("[%s].flush_io_errs   %llu\n",
				       canonical_path,
				       (unsigned long long) args.values[
					BTRFS_DEV_STAT_FLUSH_ERRS]);
			if (args.nr_items >= BTRFS_DEV_STAT_CORRUPTION_ERRS + 1)
				printf("[%s].corruption_errs %llu\n",
				       canonical_path,
				       (unsigned long long) args.values[
					BTRFS_DEV_STAT_CORRUPTION_ERRS]);
			if (args.nr_items >= BTRFS_DEV_STAT_GENERATION_ERRS + 1)
				printf("[%s].generation_errs %llu\n",
				       canonical_path,
				       (unsigned long long) args.values[
					BTRFS_DEV_STAT_GENERATION_ERRS]);

			free(canonical_path);
		}
	}

out:
	free(di_args);
	close_file_or_dir(fdmnt, dirstream);
	btrfs_close_all_devices();

	return err;
}

static const char * const cmd_device_usage_usage[] = {
	"btrfs device usage [options] <path> [<path>..]",
	"Show detailed information about internal allocations in devices.",
	HELPINFO_OUTPUT_UNIT_DF,
	NULL
};

static int _cmd_device_usage(int fd, char *path, unsigned unit_mode)
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
		printf("%s, ID: %llu\n", devinfo[i].path, devinfo[i].devid);
		print_device_sizes(fd, &devinfo[i], unit_mode);
		print_device_chunks(fd, &devinfo[i], chunkinfo, chunkcount,
				unit_mode);
		printf("\n");
	}

out:
	free(devinfo);
	free(chunkinfo);

	return ret;
}

static int cmd_device_usage(int argc, char **argv)
{
	unsigned unit_mode;
	int ret = 0;
	int more_than_one = 0;
	int i;

	unit_mode = get_unit_mode_from_arg(&argc, argv, 1);

	if (check_argc_min(argc, 2) || argv[1][0] == '-')
		usage(cmd_device_usage_usage);

	for (i = 1; i < argc; i++) {
		int fd;
		DIR *dirstream = NULL;

		if (more_than_one)
			printf("\n");

		fd = btrfs_open_dir(argv[i], &dirstream, 1);
		if (fd < 0) {
			ret = 1;
			goto out;
		}

		ret = _cmd_device_usage(fd, argv[i], unit_mode);
		close_file_or_dir(fd, dirstream);

		if (ret)
			goto out;
		more_than_one = 1;
	}
out:
	return !!ret;
}

static const char device_cmd_group_info[] =
"manage and query devices in the filesystem";

const struct cmd_group device_cmd_group = {
	device_cmd_group_usage, device_cmd_group_info, {
		{ "add", cmd_device_add, cmd_device_add_usage, NULL, 0 },
		{ "delete", cmd_device_delete, cmd_device_delete_usage, NULL,
			CMD_ALIAS },
		{ "remove", cmd_device_remove, cmd_device_remove_usage, NULL, 0 },
		{ "scan", cmd_device_scan, cmd_device_scan_usage, NULL, 0 },
		{ "ready", cmd_device_ready, cmd_device_ready_usage, NULL, 0 },
		{ "stats", cmd_device_stats, cmd_device_stats_usage, NULL, 0 },
		{ "usage", cmd_device_usage,
			cmd_device_usage_usage, NULL, 0 },
		NULL_CMD_STRUCT
	}
};

int cmd_device(int argc, char **argv)
{
	return handle_command_group(&device_cmd_group, argc, argv);
}
