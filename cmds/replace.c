/*
 * Copyright (C) 2012 STRATO.  All rights reserved.
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
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <sys/ioctl.h>
#include <errno.h>
#include <sys/stat.h>
#include <time.h>
#include <assert.h>
#include <inttypes.h>
#include <sys/wait.h>
#include <getopt.h>

#include "kerncompat.h"
#include "kernel-shared/ctree.h"
#include "ioctl.h"
#include "common/utils.h"
#include "kernel-shared/volumes.h"
#include "kernel-shared/disk-io.h"

#include "cmds/commands.h"
#include "common/help.h"
#include "common/path-utils.h"
#include "common/device-utils.h"
#include "mkfs/common.h"

static int print_replace_status(int fd, const char *path, int once);
static char *time2string(char *buf, size_t s, __u64 t);
static char *progress2string(char *buf, size_t s, int progress_1000);


static const char *replace_dev_result2string(__u64 result)
{
	switch (result) {
	case BTRFS_IOCTL_DEV_REPLACE_RESULT_NO_ERROR:
		return "no error";
	case BTRFS_IOCTL_DEV_REPLACE_RESULT_NOT_STARTED:
		return "not started";
	case BTRFS_IOCTL_DEV_REPLACE_RESULT_ALREADY_STARTED:
		return "already started";
	case BTRFS_IOCTL_DEV_REPLACE_RESULT_SCRUB_INPROGRESS:
		return "scrub is in progress";
	default:
		return "<illegal result value>";
	}
}

static const char * const replace_cmd_group_usage[] = {
	"btrfs replace <command> [<args>]",
	NULL
};

static int dev_replace_cancel_fd = -1;
static void dev_replace_sigint_handler(int signal)
{
	int ret;
	struct btrfs_ioctl_dev_replace_args args = {0};

	args.cmd = BTRFS_IOCTL_DEV_REPLACE_CMD_CANCEL;
	ret = ioctl(dev_replace_cancel_fd, BTRFS_IOC_DEV_REPLACE, &args);
	if (ret < 0)
		perror("Device replace cancel failed");
}

static int dev_replace_handle_sigint(int fd)
{
	struct sigaction sa = {
		.sa_handler = fd == -1 ? SIG_DFL : dev_replace_sigint_handler
	};

	dev_replace_cancel_fd = fd;
	return sigaction(SIGINT, &sa, NULL);
}

static const char *const cmd_replace_start_usage[] = {
	"btrfs replace start [-Bfr] <srcdev>|<devid> <targetdev> <mount_point>",
	"Replace device of a btrfs filesystem.",
	"On a live filesystem, duplicate the data to the target device which",
	"is currently stored on the source device. If the source device is not",
	"available anymore, or if the -r option is set, the data is built",
	"only using the RAID redundancy mechanisms. After completion of the",
	"operation, the source device is removed from the filesystem.",
	"If the <srcdev> is a numerical value, it is assumed to be the device id",
	"of the filesystem which is mounted at <mount_point>, otherwise it is",
	"the path to the source device. If the source device is disconnected,",
	"from the system, you have to use the <devid> parameter format.",
	"The <targetdev> needs to be same size or larger than the <srcdev>.",
	"",
	"-r     only read from <srcdev> if no other zero-defect mirror exists",
	"       (enable this if your drive has lots of read errors, the access",
	"       would be very slow)",
	"-f     force using and overwriting <targetdev> even if it looks like",
	"       containing a valid btrfs filesystem. A valid filesystem is",
	"       assumed if a btrfs superblock is found which contains a",
	"       correct checksum. Devices which are currently mounted are",
	"       never allowed to be used as the <targetdev>",
	"-B     do not background",
	"--enqueue    wait if there's another exclusive operation running,",
	"             otherwise continue",
	NULL
};

static int cmd_replace_start(const struct cmd_struct *cmd,
			     int argc, char **argv)
{
	struct btrfs_ioctl_dev_replace_args start_args = {0};
	struct btrfs_ioctl_dev_replace_args status_args = {0};
	int ret;
	int i;
	int fdmnt = -1;
	int fddstdev = -1;
	char *path;
	char *srcdev;
	char *dstdev = NULL;
	int avoid_reading_from_srcdev = 0;
	int force_using_targetdev = 0;
	u64 dstdev_block_count;
	int do_not_background = 0;
	DIR *dirstream = NULL;
	u64 srcdev_size;
	u64 dstdev_size;
	bool enqueue = false;

	optind = 0;
	while (1) {
		int c;
		enum { GETOPT_VAL_ENQUEUE = 256 };
		static const struct option long_options[] = {
			{ "enqueue", no_argument, NULL, GETOPT_VAL_ENQUEUE},
			{ NULL, 0, NULL, 0}
		};

		c = getopt_long(argc, argv, "Brf", long_options, NULL);
		if (c < 0)
			break;
		switch (c) {
		case 'B':
			do_not_background = 1;
			break;
		case 'r':
			avoid_reading_from_srcdev = 1;
			break;
		case 'f':
			force_using_targetdev = 1;
			break;
		case GETOPT_VAL_ENQUEUE:
			enqueue = true;
			break;
		default:
			usage_unknown_option(cmd, argv);
		}
	}

	start_args.start.cont_reading_from_srcdev_mode =
		avoid_reading_from_srcdev ?
		 BTRFS_IOCTL_DEV_REPLACE_CONT_READING_FROM_SRCDEV_MODE_AVOID :
		 BTRFS_IOCTL_DEV_REPLACE_CONT_READING_FROM_SRCDEV_MODE_ALWAYS;
	if (check_argc_exact(argc - optind, 3))
		return 1;
	path = argv[optind + 2];

	fdmnt = open_path_or_dev_mnt(path, &dirstream, 1);
	if (fdmnt < 0)
		goto leave_with_error;

	/* check for possible errors before backgrounding */
	status_args.cmd = BTRFS_IOCTL_DEV_REPLACE_CMD_STATUS;
	status_args.result = BTRFS_IOCTL_DEV_REPLACE_RESULT_NO_RESULT;
	ret = ioctl(fdmnt, BTRFS_IOC_DEV_REPLACE, &status_args);
	if (ret < 0) {
		fprintf(stderr,
			"ERROR: ioctl(DEV_REPLACE_STATUS) failed on \"%s\": %m",
			path);
		if (status_args.result != BTRFS_IOCTL_DEV_REPLACE_RESULT_NO_RESULT)
			fprintf(stderr, ", %s\n",
				replace_dev_result2string(status_args.result));
		else
			fprintf(stderr, "\n");
		goto leave_with_error;
	}

	if (status_args.result != BTRFS_IOCTL_DEV_REPLACE_RESULT_NO_ERROR) {
		error("ioctl(DEV_REPLACE_STATUS) on '%s' returns error: %s",
			path, replace_dev_result2string(status_args.result));
		goto leave_with_error;
	}

	if (status_args.status.replace_state ==
	    BTRFS_IOCTL_DEV_REPLACE_STATE_STARTED) {
		error("device replace on '%s' already started", path);
		goto leave_with_error;
	}

	srcdev = argv[optind];
	dstdev = path_canonicalize(argv[optind + 1]);
	if (!dstdev) {
		error("cannot canonicalize path '%s': %m",
			argv[optind + 1]);
		goto leave_with_error;
	}

	if (string_is_numerical(srcdev)) {
		struct btrfs_ioctl_fs_info_args fi_args;
		struct btrfs_ioctl_dev_info_args *di_args = NULL;

		start_args.start.srcdevid = arg_strtou64(srcdev);

		ret = get_fs_info(path, &fi_args, &di_args);
		if (ret) {
			errno = -ret;
			error("failed to get device info: %m");
			free(di_args);
			goto leave_with_error;
		}
		if (!fi_args.num_devices) {
			error("no devices found");
			free(di_args);
			goto leave_with_error;
		}

		for (i = 0; i < fi_args.num_devices; i++)
			if (start_args.start.srcdevid == di_args[i].devid)
				break;
		srcdev_size = di_args[i].total_bytes;
		free(di_args);
		if (i == fi_args.num_devices) {
			error("'%s' is not a valid devid for filesystem '%s'",
				srcdev, path);
			goto leave_with_error;
		}
	} else if (path_is_block_device(srcdev) > 0) {
		strncpy((char *)start_args.start.srcdev_name, srcdev,
			BTRFS_DEVICE_PATH_NAME_MAX);
		start_args.start.srcdevid = 0;
		srcdev_size = get_partition_size(srcdev);
	} else {
		error("source device must be a block device or a devid");
		goto leave_with_error;
	}

	ret = test_dev_for_mkfs(dstdev, force_using_targetdev);
	if (ret)
		goto leave_with_error;

	dstdev_size = get_partition_size(dstdev);
	if (srcdev_size > dstdev_size) {
		error("target device smaller than source device (required %llu bytes)",
			srcdev_size);
		goto leave_with_error;
	}

	fddstdev = open(dstdev, O_RDWR);
	if (fddstdev < 0) {
		error("unable to open %s: %m", dstdev);
		goto leave_with_error;
	}

	/* Check status before any potentially destructive operation */
	ret = check_running_fs_exclop(fdmnt, BTRFS_EXCLOP_DEV_REPLACE, enqueue);
	if (ret != 0) {
		if (ret < 0)
			error("unable to check status of exclusive operation: %m");
		close_file_or_dir(fdmnt, dirstream);
		goto leave_with_error;
	}

	strncpy((char *)start_args.start.tgtdev_name, dstdev,
		BTRFS_DEVICE_PATH_NAME_MAX);
	ret = btrfs_prepare_device(fddstdev, dstdev, &dstdev_block_count, 0,
			PREP_DEVICE_ZERO_END | PREP_DEVICE_VERBOSE);
	if (ret)
		goto leave_with_error;

	close(fddstdev);
	fddstdev = -1;
	free(dstdev);
	dstdev = NULL;

	dev_replace_handle_sigint(fdmnt);
	if (!do_not_background) {
		if (daemon(0, 0) < 0) {
			error("backgrounding failed: %m");
			goto leave_with_error;
		}
	}

	start_args.cmd = BTRFS_IOCTL_DEV_REPLACE_CMD_START;
	start_args.result = BTRFS_IOCTL_DEV_REPLACE_RESULT_NO_RESULT;
	ret = ioctl(fdmnt, BTRFS_IOC_DEV_REPLACE, &start_args);
	if (do_not_background) {
		if (ret < 0) {
			fprintf(stderr,
				"ERROR: ioctl(DEV_REPLACE_START) failed on \"%s\": %m",
				path);
			if (start_args.result != BTRFS_IOCTL_DEV_REPLACE_RESULT_NO_RESULT)
				fprintf(stderr, ", %s\n",
					replace_dev_result2string(start_args.result));
			else
				fprintf(stderr, "\n");

			if (errno == EOPNOTSUPP)
				warning("device replace of RAID5/6 not supported with this kernel");

			goto leave_with_error;
		}

		if (ret > 0) {
			error("ioctl(DEV_REPLACE_START) '%s': %s", path,
			      btrfs_err_str(ret));
			goto leave_with_error;
		}

		if (start_args.result != BTRFS_IOCTL_DEV_REPLACE_RESULT_NO_RESULT &&
		    start_args.result != BTRFS_IOCTL_DEV_REPLACE_RESULT_NO_ERROR) {
			error("ioctl(DEV_REPLACE_START) on '%s' returns error: %s",
				path,
				replace_dev_result2string(start_args.result));
			goto leave_with_error;
		}
	}
	close_file_or_dir(fdmnt, dirstream);
	return 0;

leave_with_error:
	if (dstdev)
		free(dstdev);
	if (fdmnt != -1)
		close(fdmnt);
	if (fddstdev != -1)
		close(fddstdev);
	return 1;
}
static DEFINE_SIMPLE_COMMAND(replace_start, "start");

static const char *const cmd_replace_status_usage[] = {
	"btrfs replace status [-1] <mount_point>",
	"Print status and progress information of a running device replace operation",
	"",
	"-1     print once instead of print continuously until the replace",
	"       operation finishes (or is canceled)",
	NULL
};

static int cmd_replace_status(const struct cmd_struct *cmd,
			      int argc, char **argv)
{
	int fd;
	int c;
	char *path;
	int once = 0;
	int ret;
	DIR *dirstream = NULL;

	optind = 0;
	while ((c = getopt(argc, argv, "1")) != -1) {
		switch (c) {
		case '1':
			once = 1;
			break;
		default:
			usage_unknown_option(cmd, argv);
		}
	}

	if (check_argc_exact(argc - optind, 1))
		return 1;

	path = argv[optind];
	fd = btrfs_open_dir(path, &dirstream, 1);
	if (fd < 0)
		return 1;

	ret = print_replace_status(fd, path, once);
	close_file_or_dir(fd, dirstream);
	return !!ret;
}
static DEFINE_SIMPLE_COMMAND(replace_status, "status");

static int print_replace_status(int fd, const char *path, int once)
{
	struct btrfs_ioctl_dev_replace_args args = {0};
	struct btrfs_ioctl_dev_replace_status_params *status;
	int ret;
	int prevent_loop = 0;
	int skip_stats;
	int num_chars;
	char string1[80];
	char string2[80];
	char string3[80];

	for (;;) {
		args.cmd = BTRFS_IOCTL_DEV_REPLACE_CMD_STATUS;
		args.result = BTRFS_IOCTL_DEV_REPLACE_RESULT_NO_RESULT;
		ret = ioctl(fd, BTRFS_IOC_DEV_REPLACE, &args);
		if (ret < 0) {
			fprintf(stderr, "ERROR: ioctl(DEV_REPLACE_STATUS) failed on \"%s\": %m",
				path);
			if (args.result != BTRFS_IOCTL_DEV_REPLACE_RESULT_NO_RESULT)
				fprintf(stderr, ", %s\n",
					replace_dev_result2string(args.result));
			else
				fprintf(stderr, "\n");
			return ret;
		}

		if (args.result != BTRFS_IOCTL_DEV_REPLACE_RESULT_NO_ERROR) {
			error("ioctl(DEV_REPLACE_STATUS) on '%s' returns error: %s",
				path,
				replace_dev_result2string(args.result));
			return -1;
		}

		status = &args.status;

		skip_stats = 0;
		num_chars = 0;
		switch (status->replace_state) {
		case BTRFS_IOCTL_DEV_REPLACE_STATE_STARTED:
			num_chars =
				printf("%s done",
				       progress2string(string3,
						       sizeof(string3),
						       status->progress_1000));
			break;
		case BTRFS_IOCTL_DEV_REPLACE_STATE_FINISHED:
			prevent_loop = 1;
			printf("Started on %s, finished on %s",
			       time2string(string1, sizeof(string1),
					   status->time_started),
			       time2string(string2, sizeof(string2),
					   status->time_stopped));
			break;
		case BTRFS_IOCTL_DEV_REPLACE_STATE_CANCELED:
			prevent_loop = 1;
			printf("Started on %s, canceled on %s at %s",
			       time2string(string1, sizeof(string1),
					   status->time_started),
			       time2string(string2, sizeof(string2),
					   status->time_stopped),
			       progress2string(string3, sizeof(string3),
					       status->progress_1000));
			break;
		case BTRFS_IOCTL_DEV_REPLACE_STATE_SUSPENDED:
			prevent_loop = 1;
			printf("Started on %s, suspended on %s at %s",
			       time2string(string1, sizeof(string1),
					   status->time_started),
			       time2string(string2, sizeof(string2),
					   status->time_stopped),
			       progress2string(string3, sizeof(string3),
					       status->progress_1000));
			break;
		case BTRFS_IOCTL_DEV_REPLACE_STATE_NEVER_STARTED:
			prevent_loop = 1;
			skip_stats = 1;
			printf("Never started");
			break;
		default:
			error("unknown status from ioctl DEV_REPLACE_STATUS on '%s': %llu",
					path, status->replace_state);
			return -EINVAL;
		}

		if (!skip_stats)
			num_chars += printf(
				", %llu write errs, %llu uncorr. read errs",
				(unsigned long long)status->num_write_errors,
				(unsigned long long)
				 status->num_uncorrectable_read_errors);
		if (once || prevent_loop) {
			printf("\n");
			break;
		}

		fflush(stdout);
		sleep(1);
		while (num_chars > 0) {
			putchar('\b');
			num_chars--;
		}
	}

	return 0;
}

static char *
time2string(char *buf, size_t s, __u64 t)
{
	struct tm t_tm;
	time_t t_time_t;

	t_time_t = (time_t)t;
	assert((__u64)t_time_t == t);
	localtime_r(&t_time_t, &t_tm);
	strftime(buf, s, "%e.%b %T", &t_tm);
	return buf;
}

static char *
progress2string(char *buf, size_t s, int progress_1000)
{
	snprintf(buf, s, "%d.%01d%%", progress_1000 / 10, progress_1000 % 10);
	assert(s > 0);
	buf[s - 1] = '\0';
	return buf;
}

static const char *const cmd_replace_cancel_usage[] = {
	"btrfs replace cancel <mount_point>",
	"Cancel a running device replace operation.",
	NULL
};

static int cmd_replace_cancel(const struct cmd_struct *cmd,
			      int argc, char **argv)
{
	struct btrfs_ioctl_dev_replace_args args = {0};
	int ret;
	int c;
	int fd;
	char *path;
	DIR *dirstream = NULL;

	optind = 0;
	while ((c = getopt(argc, argv, "")) != -1) {
		switch (c) {
		case '?':
		default:
			usage_unknown_option(cmd, argv);
		}
	}

	if (check_argc_exact(argc - optind, 1))
		return 1;

	path = argv[optind];
	fd = btrfs_open_dir(path, &dirstream, 1);
	if (fd < 0)
		return 1;

	args.cmd = BTRFS_IOCTL_DEV_REPLACE_CMD_CANCEL;
	args.result = BTRFS_IOCTL_DEV_REPLACE_RESULT_NO_RESULT;
	ret = ioctl(fd, BTRFS_IOC_DEV_REPLACE, &args);
	close_file_or_dir(fd, dirstream);
	if (ret < 0) {
		fprintf(stderr, "ERROR: ioctl(DEV_REPLACE_CANCEL) failed on \"%s\": %m",
			path);
		if (args.result != BTRFS_IOCTL_DEV_REPLACE_RESULT_NO_RESULT)
			fprintf(stderr, ", %s\n",
				replace_dev_result2string(args.result));
		else
			fprintf(stderr, "\n");
		return 1;
	}
	if (args.result == BTRFS_IOCTL_DEV_REPLACE_RESULT_NOT_STARTED) {
		printf("INFO: ioctl(DEV_REPLACE_CANCEL)\"%s\": %s\n",
			path, replace_dev_result2string(args.result));
		return 2;
	}
	return 0;
}
static DEFINE_SIMPLE_COMMAND(replace_cancel, "cancel");

static const char replace_cmd_group_info[] =
"replace a device in the filesystem";

static const struct cmd_group replace_cmd_group = {
	replace_cmd_group_usage, replace_cmd_group_info, {
		&cmd_struct_replace_start,
		&cmd_struct_replace_status,
		&cmd_struct_replace_cancel,
		NULL
	}
};

DEFINE_GROUP_COMMAND_TOKEN(replace);
