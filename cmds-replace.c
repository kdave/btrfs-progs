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

#include "kerncompat.h"
#include "ctree.h"
#include "ioctl.h"
#include "utils.h"
#include "volumes.h"
#include "disk-io.h"

#include "commands.h"


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
	default:
		return "<illegal result value>";
	}
}

static const char * const replace_cmd_group_usage[] = {
	"btrfs replace <command> [<args>]",
	NULL
};

static int is_numerical(const char *str)
{
	if (!(*str >= '0' && *str <= '9'))
		return 0;
	while (*str >= '0' && *str <= '9')
		str++;
	if (*str != '\0')
		return 0;
	return 1;
}

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

static const char *const cmd_start_replace_usage[] = {
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
	NULL
};

static int cmd_start_replace(int argc, char **argv)
{
	struct btrfs_ioctl_dev_replace_args start_args = {0};
	struct btrfs_ioctl_dev_replace_args status_args = {0};
	int ret;
	int i;
	int c;
	int fdmnt = -1;
	int fdsrcdev = -1;
	int fddstdev = -1;
	char *path;
	char *srcdev;
	char *dstdev;
	int avoid_reading_from_srcdev = 0;
	int force_using_targetdev = 0;
	struct stat st;
	u64 dstdev_block_count;
	int do_not_background = 0;
	int mixed = 0;
	DIR *dirstream = NULL;
	char estr[100]; /* check test_dev_for_mkfs() for error string size*/

	while ((c = getopt(argc, argv, "Brf")) != -1) {
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
		case '?':
		default:
			usage(cmd_start_replace_usage);
		}
	}

	start_args.start.cont_reading_from_srcdev_mode =
		avoid_reading_from_srcdev ?
		 BTRFS_IOCTL_DEV_REPLACE_CONT_READING_FROM_SRCDEV_MODE_AVOID :
		 BTRFS_IOCTL_DEV_REPLACE_CONT_READING_FROM_SRCDEV_MODE_ALWAYS;
	if (check_argc_exact(argc - optind, 3))
		usage(cmd_start_replace_usage);
	path = argv[optind + 2];

	fdmnt = open_path_or_dev_mnt(path, &dirstream);

	if (fdmnt < 0) {
		fprintf(stderr, "ERROR: can't access \"%s\": %s\n",
			path, strerror(errno));
		goto leave_with_error;
	}

	/* check for possible errors before backgrounding */
	status_args.cmd = BTRFS_IOCTL_DEV_REPLACE_CMD_STATUS;
	ret = ioctl(fdmnt, BTRFS_IOC_DEV_REPLACE, &status_args);
	if (ret) {
		fprintf(stderr,
			"ERROR: ioctl(DEV_REPLACE_STATUS) failed on \"%s\": %s, %s\n",
			path, strerror(errno),
			replace_dev_result2string(status_args.result));
		goto leave_with_error;
	}

	if (status_args.result != BTRFS_IOCTL_DEV_REPLACE_RESULT_NO_ERROR) {
		fprintf(stderr,
			"ERROR: ioctl(DEV_REPLACE_STATUS) on \"%s\" returns error: %s\n",
			path, replace_dev_result2string(status_args.result));
		goto leave_with_error;
	}

	if (status_args.status.replace_state ==
	    BTRFS_IOCTL_DEV_REPLACE_STATE_STARTED) {
		fprintf(stderr,
			"ERROR: btrfs replace on \"%s\" already started!\n",
			path);
		goto leave_with_error;
	}

	srcdev = argv[optind];
	dstdev = argv[optind + 1];

	if (is_numerical(srcdev)) {
		struct btrfs_ioctl_fs_info_args fi_args;
		struct btrfs_ioctl_dev_info_args *di_args = NULL;

		start_args.start.srcdevid = arg_strtou64(srcdev);

		ret = get_fs_info(path, &fi_args, &di_args);
		if (ret) {
			fprintf(stderr, "ERROR: getting dev info for devstats failed: "
					"%s\n", strerror(-ret));
			free(di_args);
			goto leave_with_error;
		}
		if (!fi_args.num_devices) {
			fprintf(stderr, "ERROR: no devices found\n");
			free(di_args);
			goto leave_with_error;
		}

		for (i = 0; i < fi_args.num_devices; i++)
			if (start_args.start.srcdevid == di_args[i].devid)
				break;
		free(di_args);
		if (i == fi_args.num_devices) {
			fprintf(stderr, "Error: '%s' is not a valid devid for filesystem '%s'\n",
				srcdev, path);
			goto leave_with_error;
		}
	} else {
		fdsrcdev = open(srcdev, O_RDWR);
		if (fdsrcdev < 0) {
			fprintf(stderr, "Error: Unable to open device '%s'\n",
				srcdev);
			fprintf(stderr, "\tTry using the devid instead of the path\n");
			goto leave_with_error;
		}
		ret = fstat(fdsrcdev, &st);
		if (ret) {
			fprintf(stderr, "Error: Unable to stat '%s'\n", srcdev);
			goto leave_with_error;
		}
		if (!S_ISBLK(st.st_mode)) {
			fprintf(stderr, "Error: '%s' is not a block device\n",
				srcdev);
			goto leave_with_error;
		}
		strncpy((char *)start_args.start.srcdev_name, srcdev,
			BTRFS_DEVICE_PATH_NAME_MAX);
		close(fdsrcdev);
		fdsrcdev = -1;
		start_args.start.srcdevid = 0;
	}

	ret = test_dev_for_mkfs(dstdev, force_using_targetdev, estr);
	if (ret) {
		fprintf(stderr, "%s", estr);
		goto leave_with_error;
	}
	fddstdev = open(dstdev, O_RDWR);
	if (fddstdev < 0) {
		fprintf(stderr, "Unable to open %s\n", dstdev);
		goto leave_with_error;
	}
	strncpy((char *)start_args.start.tgtdev_name, dstdev,
		BTRFS_DEVICE_PATH_NAME_MAX);
	ret = btrfs_prepare_device(fddstdev, dstdev, 1, &dstdev_block_count, 0,
				 &mixed, 0);
	if (ret)
		goto leave_with_error;

	close(fddstdev);
	fddstdev = -1;

	dev_replace_handle_sigint(fdmnt);
	if (!do_not_background) {
		if (daemon(0, 0) < 0) {
			fprintf(stderr, "ERROR, backgrounding failed: %s\n",
				strerror(errno));
			goto leave_with_error;
		}
	}

	start_args.cmd = BTRFS_IOCTL_DEV_REPLACE_CMD_START;
	ret = ioctl(fdmnt, BTRFS_IOC_DEV_REPLACE, &start_args);
	if (do_not_background) {
		if (ret) {
			fprintf(stderr,
				"ERROR: ioctl(DEV_REPLACE_START) failed on \"%s\": %s, %s\n",
				path, strerror(errno),
				replace_dev_result2string(start_args.result));
			goto leave_with_error;
		}

		if (start_args.result !=
		    BTRFS_IOCTL_DEV_REPLACE_RESULT_NO_ERROR) {
			fprintf(stderr,
				"ERROR: ioctl(DEV_REPLACE_START) on \"%s\" returns error: %s\n",
				path,
				replace_dev_result2string(start_args.result));
			goto leave_with_error;
		}
	}
	close_file_or_dir(fdmnt, dirstream);
	return 0;

leave_with_error:
	if (fdmnt != -1)
		close(fdmnt);
	if (fdsrcdev != -1)
		close(fdsrcdev);
	if (fddstdev != -1)
		close(fddstdev);
	return 1;
}

static const char *const cmd_status_replace_usage[] = {
	"btrfs replace status [-1] <mount_point>",
	"Print status and progress information of a running device replace",
	"operation",
	"",
	"-1     print once instead of print continuously until the replace",
	"       operation finishes (or is canceled)",
	NULL
};

static int cmd_status_replace(int argc, char **argv)
{
	int fd;
	int e;
	int c;
	char *path;
	int once = 0;
	int ret;
	DIR *dirstream = NULL;

	while ((c = getopt(argc, argv, "1")) != -1) {
		switch (c) {
		case '1':
			once = 1;
			break;
		case '?':
		default:
			usage(cmd_status_replace_usage);
		}
	}

	if (check_argc_exact(argc - optind, 1))
		usage(cmd_status_replace_usage);

	path = argv[optind];
	fd = open_file_or_dir(path, &dirstream);
	e = errno;
	if (fd < 0) {
		fprintf(stderr, "ERROR: can't access \"%s\": %s\n",
			path, strerror(e));
		return 1;
	}

	ret = print_replace_status(fd, path, once);
	close_file_or_dir(fd, dirstream);
	return !!ret;
}

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
		ret = ioctl(fd, BTRFS_IOC_DEV_REPLACE, &args);
		if (ret) {
			fprintf(stderr, "ERROR: ioctl(DEV_REPLACE_STATUS) failed on \"%s\": %s, %s\n",
				path, strerror(errno),
				replace_dev_result2string(args.result));
			return ret;
		}

		status = &args.status;
		if (args.result != BTRFS_IOCTL_DEV_REPLACE_RESULT_NO_ERROR) {
			fprintf(stderr, "ERROR: ioctl(DEV_REPLACE_STATUS) on \"%s\" returns error: %s\n",
				path,
				replace_dev_result2string(args.result));
			return -1;
		}

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
			prevent_loop = 1;
			assert(0);
			break;
		}

		if (!skip_stats)
			num_chars += printf(
				", %llu write errs, %llu uncorr. read errs",
				(unsigned long long)status->num_write_errors,
				(unsigned long long)
				 status->num_uncorrectable_read_errors);
		if (once || prevent_loop) {
			printf("\n");
			return 0;
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

static const char *const cmd_cancel_replace_usage[] = {
	"btrfs replace cancel <mount_point>",
	"Cancel a running device replace operation.",
	NULL
};

static int cmd_cancel_replace(int argc, char **argv)
{
	struct btrfs_ioctl_dev_replace_args args = {0};
	int ret;
	int c;
	int fd;
	int e;
	char *path;
	DIR *dirstream = NULL;

	while ((c = getopt(argc, argv, "")) != -1) {
		switch (c) {
		case '?':
		default:
			usage(cmd_cancel_replace_usage);
		}
	}

	if (check_argc_exact(argc - optind, 1))
		usage(cmd_cancel_replace_usage);

	path = argv[optind];
	fd = open_file_or_dir(path, &dirstream);
	if (fd < 0) {
		fprintf(stderr, "ERROR: can't access \"%s\": %s\n",
			path, strerror(errno));
		return 1;
	}

	args.cmd = BTRFS_IOCTL_DEV_REPLACE_CMD_CANCEL;
	ret = ioctl(fd, BTRFS_IOC_DEV_REPLACE, &args);
	e = errno;
	close_file_or_dir(fd, dirstream);
	if (ret) {
		fprintf(stderr, "ERROR: ioctl(DEV_REPLACE_CANCEL) failed on \"%s\": %s, %s\n",
			path, strerror(e),
			replace_dev_result2string(args.result));
		return 1;
	}
	if (args.result == BTRFS_IOCTL_DEV_REPLACE_RESULT_NOT_STARTED) {
		printf("INFO: ioctl(DEV_REPLACE_CANCEL)\"%s\": %s\n",
			path, replace_dev_result2string(args.result));
		return 2;
	}
	return 0;
}

const struct cmd_group replace_cmd_group = {
	replace_cmd_group_usage, NULL, {
		{ "start", cmd_start_replace, cmd_start_replace_usage, NULL,
		  0 },
		{ "status", cmd_status_replace, cmd_status_replace_usage, NULL,
		  0 },
		{ "cancel", cmd_cancel_replace, cmd_cancel_replace_usage, NULL,
		  0 },
		NULL_CMD_STRUCT
	}
};

int cmd_replace(int argc, char **argv)
{
	return handle_command_group(&replace_cmd_group, argc, argv);
}
