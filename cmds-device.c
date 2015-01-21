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
#include "cmds-fi-disk_usage.h"

#include "commands.h"

static const char * const device_cmd_group_usage[] = {
	"btrfs device <command> [<args>]",
	NULL
};

static const char * const cmd_add_dev_usage[] = {
	"btrfs device add [options] <device> [<device>...] <path>",
	"Add a device to a filesystem",
	"-K|--nodiscard    do not perform whole device TRIM",
	"-f|--force        force overwrite existing filesystem on the disk",
	NULL
};

static int cmd_add_dev(int argc, char **argv)
{
	char	*mntpnt;
	int	i, fdmnt, ret=0, e;
	DIR	*dirstream = NULL;
	int discard = 1;
	int force = 0;
	char estr[100];

	while (1) {
		int long_index;
		static const struct option long_options[] = {
			{ "nodiscard", optional_argument, NULL, 'K'},
			{ "force", no_argument, NULL, 'f'},
			{ NULL, 0, NULL, 0}
		};
		int c = getopt_long(argc, argv, "Kf", long_options,
					&long_index);
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
			usage(cmd_add_dev_usage);
		}
	}

	argc = argc - optind;

	if (check_argc_min(argc, 2))
		usage(cmd_add_dev_usage);

	mntpnt = argv[optind + argc - 1];

	fdmnt = open_file_or_dir(mntpnt, &dirstream);
	if (fdmnt < 0) {
		fprintf(stderr, "ERROR: can't access '%s'\n", mntpnt);
		return 1;
	}

	for (i = optind; i < optind + argc - 1; i++){
		struct btrfs_ioctl_vol_args ioctl_args;
		int	devfd, res;
		u64 dev_block_count = 0;
		int mixed = 0;
		char *path;

		res = test_dev_for_mkfs(argv[i], force, estr);
		if (res) {
			fprintf(stderr, "%s", estr);
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
	return !!ret;
}

static const char * const cmd_rm_dev_usage[] = {
	"btrfs device delete <device> [<device>...] <path>",
	"Remove a device from a filesystem",
	NULL
};

static int cmd_rm_dev(int argc, char **argv)
{
	char	*mntpnt;
	int	i, fdmnt, ret=0, e;
	DIR	*dirstream = NULL;

	if (check_argc_min(argc, 3))
		usage(cmd_rm_dev_usage);

	mntpnt = argv[argc - 1];

	fdmnt = open_file_or_dir(mntpnt, &dirstream);
	if (fdmnt < 0) {
		fprintf(stderr, "ERROR: can't access '%s'\n", mntpnt);
		return 1;
	}

	for(i=1 ; i < argc - 1; i++ ){
		struct	btrfs_ioctl_vol_args arg;
		int	res;

		if (!is_block_device(argv[i])) {
			fprintf(stderr,
				"ERROR: %s is not a block device\n", argv[i]);
			ret++;
			continue;
		}
		strncpy_null(arg.name, argv[i]);
		res = ioctl(fdmnt, BTRFS_IOC_RM_DEV, &arg);
		e = errno;
		if (res > 0) {
			fprintf(stderr,
				"ERROR: error removing the device '%s' - %s\n",
				argv[i], btrfs_err_str(res));
			ret++;
		} else if (res < 0) {
			fprintf(stderr,
				"ERROR: error removing the device '%s' - %s\n",
				argv[i], strerror(e));
			ret++;
		}
	}

	close_file_or_dir(fdmnt, dirstream);
	return !!ret;
}

static const char * const cmd_scan_dev_usage[] = {
	"btrfs device scan [(-d|--all-devices)|<device> [<device>...]]",
	"Scan devices for a btrfs filesystem",
	" -d|--all-devices (deprecated)",
	NULL
};

static int cmd_scan_dev(int argc, char **argv)
{
	int i;
	int devstart = 1;
	int all = 0;
	int ret = 0;

	optind = 1;
	while (1) {
		int long_index;
		static const struct option long_options[] = {
			{ "all-devices", no_argument, NULL, 'd'},
			{ NULL, 0, NULL, 0}
		};
		int c = getopt_long(argc, argv, "d", long_options,
				    &long_index);
		if (c < 0)
			break;
		switch (c) {
		case 'd':
			all = 1;
			break;
		default:
			usage(cmd_scan_dev_usage);
		}
	}

	if (all && check_argc_max(argc, 2))
		usage(cmd_scan_dev_usage);

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

		if (!is_block_device(argv[i])) {
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
	return !!ret;
}

static const char * const cmd_ready_dev_usage[] = {
	"btrfs device ready <device>",
	"Check device to see if it has all of its devices in cache for mounting",
	NULL
};

static int cmd_ready_dev(int argc, char **argv)
{
	struct	btrfs_ioctl_vol_args args;
	int	fd;
	int	ret;
	char	*path;

	if (check_argc_min(argc, 2))
		usage(cmd_ready_dev_usage);

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

	if (!is_block_device(path)) {
		fprintf(stderr,
			"ERROR: %s is not a block device\n", path);
		ret = 1;
		goto out;
	}

	strncpy(args.name, path, BTRFS_PATH_NAME_MAX);
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

static const char * const cmd_dev_stats_usage[] = {
	"btrfs device stats [-z] <path>|<device>",
	"Show current device IO stats. -z to reset stats afterwards.",
	NULL
};

static int cmd_dev_stats(int argc, char **argv)
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
			usage(cmd_dev_stats_usage);
		}
	}

	argc = argc - optind;
	if (check_argc_exact(argc, 1))
		usage(cmd_dev_stats_usage);

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

	return err;
}

const char * const cmd_device_usage_usage[] = {
	"btrfs device usage [options] <path> [<path>..]",
	"Show detailed information about internal allocations in devices.",
	"-b|--raw           raw numbers in bytes",
	"-h|--human-readable",
	"                   human friendly numbers, base 1024 (default)",
	"-H                 human friendly numbers, base 1000",
	"--iec              use 1024 as a base (KiB, MiB, GiB, TiB)",
	"--si               use 1000 as a base (kB, MB, GB, TB)",
	"-k|--kbytes        show sizes in KiB, or kB with --si",
	"-m|--mbytes        show sizes in MiB, or MB with --si",
	"-g|--gbytes        show sizes in GiB, or GB with --si",
	"-t|--tbytes        show sizes in TiB, or TB with --si",
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

int cmd_device_usage(int argc, char **argv)
{
	unsigned unit_mode = UNITS_DEFAULT;
	int ret = 0;
	int	i, more_than_one = 0;

	optind = 1;
	while (1) {
		int long_index;
		static const struct option long_options[] = {
			{ "raw", no_argument, NULL, 'b'},
			{ "kbytes", no_argument, NULL, 'k'},
			{ "mbytes", no_argument, NULL, 'm'},
			{ "gbytes", no_argument, NULL, 'g'},
			{ "tbytes", no_argument, NULL, 't'},
			{ "si", no_argument, NULL, GETOPT_VAL_SI},
			{ "iec", no_argument, NULL, GETOPT_VAL_IEC},
			{ "human-readable", no_argument, NULL,
				GETOPT_VAL_HUMAN_READABLE},
			{ NULL, 0, NULL, 0 }
		};
		int c = getopt_long(argc, argv, "bhHkmgt", long_options,
				&long_index);

		if (c < 0)
			break;
		switch (c) {
		case 'b':
			unit_mode = UNITS_RAW;
			break;
		case 'k':
			units_set_base(&unit_mode, UNITS_KBYTES);
			break;
		case 'm':
			units_set_base(&unit_mode, UNITS_MBYTES);
			break;
		case 'g':
			units_set_base(&unit_mode, UNITS_GBYTES);
			break;
		case 't':
			units_set_base(&unit_mode, UNITS_TBYTES);
			break;
		case GETOPT_VAL_HUMAN_READABLE:
		case 'h':
			unit_mode = UNITS_HUMAN_BINARY;
			break;
		case 'H':
			unit_mode = UNITS_HUMAN_DECIMAL;
			break;
		case GETOPT_VAL_SI:
			units_set_mode(&unit_mode, UNITS_DECIMAL);
			break;
		case GETOPT_VAL_IEC:
			units_set_mode(&unit_mode, UNITS_BINARY);
			break;
		default:
			usage(cmd_device_usage_usage);
		}
	}

	if (check_argc_min(argc - optind, 1))
		usage(cmd_device_usage_usage);

	for (i = optind; i < argc ; i++) {
		int fd;
		DIR	*dirstream = NULL;
		if (more_than_one)
			printf("\n");

		fd = open_file_or_dir(argv[i], &dirstream);
		if (fd < 0) {
			fprintf(stderr, "ERROR: can't access '%s'\n",
				argv[1]);
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

const struct cmd_group device_cmd_group = {
	device_cmd_group_usage, NULL, {
		{ "add", cmd_add_dev, cmd_add_dev_usage, NULL, 0 },
		{ "delete", cmd_rm_dev, cmd_rm_dev_usage, NULL, 0 },
		{ "scan", cmd_scan_dev, cmd_scan_dev_usage, NULL, 0 },
		{ "ready", cmd_ready_dev, cmd_ready_dev_usage, NULL, 0 },
		{ "stats", cmd_dev_stats, cmd_dev_stats_usage, NULL, 0 },
		{ "usage", cmd_device_usage,
			cmd_device_usage_usage, NULL, 0 },
		NULL_CMD_STRUCT
	}
};

int cmd_device(int argc, char **argv)
{
	return handle_command_group(&device_cmd_group, argc, argv);
}
