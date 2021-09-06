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
#include <getopt.h>
#include <sys/ioctl.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <errno.h>

#include "kerncompat.h"
#include "kernel-shared/ctree.h"
#include "ioctl.h"
#include "kernel-shared/volumes.h"
#include "common/open-utils.h"
#include "cmds/commands.h"
#include "common/utils.h"
#include "common/parse-utils.h"
#include "common/help.h"

static const char * const balance_cmd_group_usage[] = {
	"btrfs balance <command> [options] <path>",
	"btrfs balance <path>",
	NULL
};

static int parse_one_profile(const char *profile, u64 *flags)
{
	int ret;
	u64 tmp = 0;

	ret = parse_bg_profile(profile, &tmp);
	if (ret) {
		error("unknown profile: %s", profile);
		return 1;
	}
	if (tmp == 0)
		tmp = BTRFS_AVAIL_ALLOC_BIT_SINGLE;
	*flags |= tmp;
	return ret;
}

static int parse_profiles(char *profiles, u64 *flags)
{
	char *this_char;
	char *save_ptr = NULL; /* Satisfy static checkers */

	for (this_char = strtok_r(profiles, "|", &save_ptr);
	     this_char != NULL;
	     this_char = strtok_r(NULL, "|", &save_ptr)) {
		if (parse_one_profile(this_char, flags))
			return 1;
	}

	return 0;
}

__attribute__ ((unused))
static void print_range(u64 start, u64 end)
{
	if (start)
		printf("%llu", (unsigned long long)start);
	printf("..");
	if (end != (u64)-1)
		printf("%llu", (unsigned long long)end);
}

__attribute__ ((unused))
static void print_range_u32(u32 start, u32 end)
{
	if (start)
		printf("%u", start);
	printf("..");
	if (end != (u32)-1)
		printf("%u", end);
}

static int parse_filters(char *filters, struct btrfs_balance_args *args)
{
	char *this_char;
	char *value;
	char *save_ptr = NULL; /* Satisfy static checkers */

	if (!filters)
		return 0;

	for (this_char = strtok_r(filters, ",", &save_ptr);
	     this_char != NULL;
	     this_char = strtok_r(NULL, ",", &save_ptr)) {
		if ((value = strchr(this_char, '=')) != NULL)
			*value++ = 0;
		if (!strcmp(this_char, "profiles")) {
			if (!value || !*value) {
				error("the profiles filter requires an argument");
				return 1;
			}
			if (parse_profiles(value, &args->profiles)) {
				error("invalid profiles argument");
				return 1;
			}
			args->flags |= BTRFS_BALANCE_ARGS_PROFILES;
		} else if (!strcmp(this_char, "usage")) {
			if (!value || !*value) {
				error("the usage filter requires an argument");
				return 1;
			}
			if (parse_u64(value, &args->usage)) {
				if (parse_range_u32(value, &args->usage_min,
							&args->usage_max)) {
					error("invalid usage argument: %s",
						value);
					return 1;
				}
				if (args->usage_max > 100) {
					error("invalid usage argument: %s",
						value);
				}
				args->flags &= ~BTRFS_BALANCE_ARGS_USAGE;
				args->flags |= BTRFS_BALANCE_ARGS_USAGE_RANGE;
			} else {
				if (args->usage > 100) {
					error("invalid usage argument: %s",
						value);
					return 1;
				}
				args->flags &= ~BTRFS_BALANCE_ARGS_USAGE_RANGE;
				args->flags |= BTRFS_BALANCE_ARGS_USAGE;
			}
			args->flags |= BTRFS_BALANCE_ARGS_USAGE;
		} else if (!strcmp(this_char, "devid")) {
			if (!value || !*value) {
				error("the devid filter requires an argument");
				return 1;
			}
			if (parse_u64(value, &args->devid) || args->devid == 0) {
				error("invalid devid argument: %s", value);
				return 1;
			}
			args->flags |= BTRFS_BALANCE_ARGS_DEVID;
		} else if (!strcmp(this_char, "drange")) {
			if (!value || !*value) {
				error("the drange filter requires an argument");
				return 1;
			}
			if (parse_range_strict(value, &args->pstart, &args->pend)) {
				error("invalid drange argument");
				return 1;
			}
			args->flags |= BTRFS_BALANCE_ARGS_DRANGE;
		} else if (!strcmp(this_char, "vrange")) {
			if (!value || !*value) {
				error("the vrange filter requires an argument");
				return 1;
			}
			if (parse_range_strict(value, &args->vstart, &args->vend)) {
				error("invalid vrange argument");
				return 1;
			}
			args->flags |= BTRFS_BALANCE_ARGS_VRANGE;
		} else if (!strcmp(this_char, "convert")) {
			if (!value || !*value) {
				error("the convert option requires an argument");
				return 1;
			}
			if (parse_one_profile(value, &args->target)) {
				error("invalid convert argument");
				return 1;
			}
			args->flags |= BTRFS_BALANCE_ARGS_CONVERT;
		} else if (!strcmp(this_char, "soft")) {
			args->flags |= BTRFS_BALANCE_ARGS_SOFT;
		} else if (!strcmp(this_char, "limit")) {
			if (!value || !*value) {
				error("the limit filter requires an argument");
				return 1;
			}
			if (parse_u64(value, &args->limit)) {
				if (parse_range_u32(value, &args->limit_min,
							&args->limit_max)) {
					error("Invalid limit argument: %s",
					       value);
					return 1;
				}
				args->flags &= ~BTRFS_BALANCE_ARGS_LIMIT;
				args->flags |= BTRFS_BALANCE_ARGS_LIMIT_RANGE;
			} else {
				args->flags &= ~BTRFS_BALANCE_ARGS_LIMIT_RANGE;
				args->flags |= BTRFS_BALANCE_ARGS_LIMIT;
			}
		} else if (!strcmp(this_char, "stripes")) {
			if (!value || !*value) {
				error("the stripes filter requires an argument");
				return 1;
			}
			if (parse_range_u32(value, &args->stripes_min,
					    &args->stripes_max)) {
				error("invalid stripes argument");
				return 1;
			}
			args->flags |= BTRFS_BALANCE_ARGS_STRIPES_RANGE;
		} else {
			error("unrecognized balance option: %s", this_char);
			return 1;
		}
	}

	return 0;
}

static void dump_balance_args(struct btrfs_balance_args *args)
{
	if (args->flags & BTRFS_BALANCE_ARGS_CONVERT) {
		printf("converting, target=%llu, soft is %s",
		       (unsigned long long)args->target,
		       (args->flags & BTRFS_BALANCE_ARGS_SOFT) ? "on" : "off");
	} else {
		printf("balancing");
	}

	if (args->flags & BTRFS_BALANCE_ARGS_PROFILES)
		printf(", profiles=%llu", (unsigned long long)args->profiles);
	if (args->flags & BTRFS_BALANCE_ARGS_USAGE)
		printf(", usage=%llu", (unsigned long long)args->usage);
	if (args->flags & BTRFS_BALANCE_ARGS_USAGE_RANGE) {
		printf(", usage=");
		print_range_u32(args->usage_min, args->usage_max);
	}
	if (args->flags & BTRFS_BALANCE_ARGS_DEVID)
		printf(", devid=%llu", (unsigned long long)args->devid);
	if (args->flags & BTRFS_BALANCE_ARGS_DRANGE)
		printf(", drange=%llu..%llu",
		       (unsigned long long)args->pstart,
		       (unsigned long long)args->pend);
	if (args->flags & BTRFS_BALANCE_ARGS_VRANGE)
		printf(", vrange=%llu..%llu",
		       (unsigned long long)args->vstart,
		       (unsigned long long)args->vend);
	if (args->flags & BTRFS_BALANCE_ARGS_LIMIT)
		printf(", limit=%llu", (unsigned long long)args->limit);
	if (args->flags & BTRFS_BALANCE_ARGS_LIMIT_RANGE) {
		printf(", limit=");
		print_range_u32(args->limit_min, args->limit_max);
	}
	if (args->flags & BTRFS_BALANCE_ARGS_STRIPES_RANGE) {
		printf(", stripes=");
		print_range_u32(args->stripes_min, args->stripes_max);
	}

	printf("\n");
}

static void dump_ioctl_balance_args(struct btrfs_ioctl_balance_args *args)
{
	printf("Dumping filters: flags 0x%llx, state 0x%llx, force is %s\n",
	       (unsigned long long)args->flags, (unsigned long long)args->state,
	       (args->flags & BTRFS_BALANCE_FORCE) ? "on" : "off");
	if (args->flags & BTRFS_BALANCE_DATA) {
		printf("  DATA (flags 0x%llx): ",
		       (unsigned long long)args->data.flags);
		dump_balance_args(&args->data);
	}
	if (args->flags & BTRFS_BALANCE_METADATA) {
		printf("  METADATA (flags 0x%llx): ",
		       (unsigned long long)args->meta.flags);
		dump_balance_args(&args->meta);
	}
	if (args->flags & BTRFS_BALANCE_SYSTEM) {
		printf("  SYSTEM (flags 0x%llx): ",
		       (unsigned long long)args->sys.flags);
		dump_balance_args(&args->sys);
	}
}

static int do_balance_v1(int fd)
{
	struct btrfs_ioctl_vol_args args;
	int ret;

	memset(&args, 0, sizeof(args));
	ret = ioctl(fd, BTRFS_IOC_BALANCE, &args);
	return ret;
}

enum {
	BALANCE_START_FILTERS = 1 << 0,
	BALANCE_START_NOWARN  = 1 << 1
};

static int do_balance(const char *path, struct btrfs_ioctl_balance_args *args,
		      unsigned flags, bool enqueue)
{
	int fd;
	int ret;
	DIR *dirstream = NULL;

	fd = btrfs_open_dir(path, &dirstream, 1);
	if (fd < 0)
		return 1;

	ret = check_running_fs_exclop(fd, BTRFS_EXCLOP_BALANCE, enqueue);
	if (ret != 0) {
		if (ret < 0)
			error("unable to check status of exclusive operation: %m");
		close_file_or_dir(fd, dirstream);
		return 1;
	}

	ret = ioctl(fd, BTRFS_IOC_BALANCE_V2, args);
	if (ret < 0) {
		/*
		 * older kernels don't have the new balance ioctl, try the
		 * old one.  But, the old one doesn't know any filters, so
		 * don't fall back if they tried to use the fancy new things
		 */
		if (errno == ENOTTY && !(flags & BALANCE_START_FILTERS)) {
			ret = do_balance_v1(fd);
			if (ret == 0)
				goto out;
		}

		if (errno == ECANCELED) {
			if (args->state & BTRFS_BALANCE_STATE_PAUSE_REQ)
				fprintf(stderr, "balance paused by user\n");
			if (args->state & BTRFS_BALANCE_STATE_CANCEL_REQ)
				fprintf(stderr, "balance canceled by user\n");
			ret = 0;
		} else {
			error("error during balancing '%s': %m", path);
			if (errno != EINPROGRESS)
				fprintf(stderr,
			"There may be more info in syslog - try dmesg | tail\n");
			ret = 1;
		}
	} else if (ret > 0) {
		error("balance: %s", btrfs_err_str(ret));
	} else {
		pr_verbose(MUST_LOG,
			   "Done, had to relocate %llu out of %llu chunks\n",
			   (unsigned long long)args->stat.completed,
			   (unsigned long long)args->stat.considered);
	}

out:
	close_file_or_dir(fd, dirstream);
	return ret;
}

static const char * const cmd_balance_start_usage[] = {
	"btrfs balance start [options] <path>",
	"Balance chunks across the devices",
	"Balance and/or convert (change allocation profile of) chunks that",
	"passed all filters in a comma-separated list of filters for a",
	"particular chunk type.  If filter list is not given balance all",
	"chunks of that type.  In case none of the -d, -m or -s options is",
	"given balance all chunks in a filesystem. This is potentially",
	"long operation and the user is warned before this start, with",
	"a delay to stop it.",
	"",
	"-d[filters]    act on data chunks",
	"-m[filters]    act on metadata chunks",
	"-s[filters]    act on system chunks (only under -f)",
	"-f             force a reduction of metadata integrity, or",
	"               skip timeout when converting to RAID56 profiles",
	"--full-balance do not print warning and do not delay start",
	"--background|--bg",
	"               run the balance as a background process",
	"--enqueue      wait if there's another exclusive operation running,",
	"               otherwise continue",
	"-v|--verbose   deprecated, alias for global -v option",
	HELPINFO_INSERT_GLOBALS,
	HELPINFO_INSERT_VERBOSE,
	HELPINFO_INSERT_QUIET,
	NULL
};

static int cmd_balance_start(const struct cmd_struct *cmd,
			     int argc, char **argv)
{
	struct btrfs_ioctl_balance_args args;
	struct btrfs_balance_args *ptrs[] = { &args.data, &args.sys,
						&args.meta, NULL };
	int force = 0;
	int background = 0;
	bool enqueue = false;
	unsigned start_flags = 0;
	bool raid56_warned = false;
	int i;

	memset(&args, 0, sizeof(args));

	optind = 0;
	while (1) {
		enum { GETOPT_VAL_FULL_BALANCE = 256,
			GETOPT_VAL_BACKGROUND = 257,
			GETOPT_VAL_ENQUEUE };
		static const struct option longopts[] = {
			{ "data", optional_argument, NULL, 'd'},
			{ "metadata", optional_argument, NULL, 'm' },
			{ "system", optional_argument, NULL, 's' },
			{ "force", no_argument, NULL, 'f' },
			{ "verbose", no_argument, NULL, 'v' },
			{ "full-balance", no_argument, NULL,
				GETOPT_VAL_FULL_BALANCE },
			{ "background", no_argument, NULL,
				GETOPT_VAL_BACKGROUND },
			{ "bg", no_argument, NULL, GETOPT_VAL_BACKGROUND },
			{ "enqueue", no_argument, NULL, GETOPT_VAL_ENQUEUE},
			{ NULL, 0, NULL, 0 }
		};

		int opt = getopt_long(argc, argv, "d::s::m::fv", longopts, NULL);
		if (opt < 0)
			break;

		switch (opt) {
		case 'd':
			start_flags |= BALANCE_START_FILTERS;
			args.flags |= BTRFS_BALANCE_DATA;

			if (parse_filters(optarg, &args.data))
				return 1;
			break;
		case 's':
			start_flags |= BALANCE_START_FILTERS;
			args.flags |= BTRFS_BALANCE_SYSTEM;

			if (parse_filters(optarg, &args.sys))
				return 1;
			break;
		case 'm':
			start_flags |= BALANCE_START_FILTERS;
			args.flags |= BTRFS_BALANCE_METADATA;

			if (parse_filters(optarg, &args.meta))
				return 1;
			break;
		case 'f':
			force = 1;
			break;
		case 'v':
			bconf_be_verbose();
			break;
		case GETOPT_VAL_FULL_BALANCE:
			start_flags |= BALANCE_START_NOWARN;
			break;
		case GETOPT_VAL_BACKGROUND:
			background = 1;
			break;
		case GETOPT_VAL_ENQUEUE:
			enqueue = true;
			break;
		default:
			usage_unknown_option(cmd, argv);
		}
	}

	if (check_argc_exact(argc - optind, 1))
		return 1;

	/*
	 * allow -s only under --force, otherwise do with system chunks
	 * the same thing we were ordered to do with meta chunks
	 */
	if (args.flags & BTRFS_BALANCE_SYSTEM) {
		if (!force) {
			error(
			    "Refusing to explicitly operate on system chunks.\n"
			    "Pass --force if you really want to do that.");
			return 1;
		}
	} else if (args.flags & BTRFS_BALANCE_METADATA) {
		args.flags |= BTRFS_BALANCE_SYSTEM;
		memcpy(&args.sys, &args.meta,
			sizeof(struct btrfs_balance_args));
	}

	if (!(start_flags & BALANCE_START_FILTERS)) {
		/* relocate everything - no filters */
		args.flags |= BTRFS_BALANCE_TYPE_MASK;
	}

	/* drange makes sense only when devid is set */
	for (i = 0; ptrs[i]; i++) {
		if ((ptrs[i]->flags & BTRFS_BALANCE_ARGS_DRANGE) &&
		    !(ptrs[i]->flags & BTRFS_BALANCE_ARGS_DEVID)) {
			error("drange filter must be used with devid filter");
			return 1;
		}
	}

	/* soft makes sense only when convert for corresponding type is set */
	for (i = 0; ptrs[i]; i++) {
		int delay = 10;

		if ((ptrs[i]->flags & BTRFS_BALANCE_ARGS_SOFT) &&
		    !(ptrs[i]->flags & BTRFS_BALANCE_ARGS_CONVERT)) {
			error("'soft' option can be used only when converting profiles");
			return 1;
		}

		if (!(ptrs[i]->flags & BTRFS_BALANCE_ARGS_CONVERT))
			continue;

		if (!(ptrs[i]->target & BTRFS_BLOCK_GROUP_RAID56_MASK))
			continue;

		if (raid56_warned)
			continue;

		raid56_warned = true;
		printf("WARNING:\n\n");
		printf("\tRAID5/6 support has known problems and is strongly discouraged\n");
		printf("\tto be used besides testing or evaluation. It is recommended that\n");
		printf("\tyou use one of the other RAID profiles.\n");
		/*
		 * Override timeout by the --force option too, though it's
		 * otherwise used for allowing redundancy reduction.
		 */
		if (force) {
			printf("\tSafety timeout skipped due to --force\n\n");
			continue;
		}
		printf("\tThe operation will continue in %d seconds.\n", delay);
		printf("\tUse Ctrl-C to stop.\n");
		while (delay) {
			printf("%2d", delay--);
			fflush(stdout);
			sleep(1);
		}
		printf("\nStarting conversion to RAID5/6.\n");
	}

	if (!(start_flags & BALANCE_START_FILTERS) && !(start_flags & BALANCE_START_NOWARN)) {
		int delay = 10;

		printf("WARNING:\n\n");
		printf("\tFull balance without filters requested. This operation is very\n");
		printf("\tintense and takes potentially very long. It is recommended to\n");
		printf("\tuse the balance filters to narrow down the scope of balance.\n");
		printf("\tUse 'btrfs balance start --full-balance' option to skip this\n");
		printf("\twarning. The operation will start in %d seconds.\n", delay);
		printf("\tUse Ctrl-C to stop it.\n");
		while (delay) {
			printf("%2d", delay--);
			fflush(stdout);
			sleep(1);
		}
		printf("\nStarting balance without any filters.\n");
	}

	if (force)
		args.flags |= BTRFS_BALANCE_FORCE;
	if (bconf.verbose > BTRFS_BCONF_QUIET)
		dump_ioctl_balance_args(&args);
	if (background) {
		switch (fork()) {
		case (-1):
			error("unable to fork to run balance in background");
			return 1;
		case (0):
			setsid();
			switch(fork()) {
			case (-1):
				error(
				"unable to fork to run balance in background");
				exit(1);
			case (0):
				/*
				 * Read the return value to silence compiler
				 * warning. Change to / should succeed and
				 * we're not in a security-sensitive context.
				 */
				i = chdir("/");
				close(0);
				close(1);
				close(2);
				open("/dev/null", O_RDONLY);
				open("/dev/null", O_WRONLY);
				open("/dev/null", O_WRONLY);
				break;
			default:
				exit(0);
			}
			break;
		default:
			exit(0);
		}
	}

	return do_balance(argv[optind], &args, start_flags, enqueue);
}
static DEFINE_SIMPLE_COMMAND(balance_start, "start");

static const char * const cmd_balance_pause_usage[] = {
	"btrfs balance pause <path>",
	"Pause running balance",
	NULL
};

static int cmd_balance_pause(const struct cmd_struct *cmd,
			     int argc, char **argv)
{
	const char *path;
	int fd;
	int ret;
	DIR *dirstream = NULL;

	clean_args_no_options(cmd, argc, argv);

	if (check_argc_exact(argc - optind, 1))
		return 1;

	path = argv[optind];

	fd = btrfs_open_dir(path, &dirstream, 1);
	if (fd < 0)
		return 1;

	ret = ioctl(fd, BTRFS_IOC_BALANCE_CTL, BTRFS_BALANCE_CTL_PAUSE);
	if (ret < 0) {
		error("balance pause on '%s' failed: %s", path,
			(errno == ENOTCONN) ? "Not running" : strerror(errno));
		if (errno == ENOTCONN)
			ret = 2;
		else
			ret = 1;
	}

	btrfs_warn_multiple_profiles(fd);
	close_file_or_dir(fd, dirstream);
	return ret;
}
static DEFINE_SIMPLE_COMMAND(balance_pause, "pause");

static const char * const cmd_balance_cancel_usage[] = {
	"btrfs balance cancel <path>",
	"Cancel running or paused balance",
	NULL
};

static int cmd_balance_cancel(const struct cmd_struct *cmd,
			      int argc, char **argv)
{
	const char *path;
	int fd;
	int ret;
	DIR *dirstream = NULL;

	clean_args_no_options(cmd, argc, argv);

	if (check_argc_exact(argc - optind, 1))
		return 1;

	path = argv[optind];

	fd = btrfs_open_dir(path, &dirstream, 1);
	if (fd < 0)
		return 1;

	ret = ioctl(fd, BTRFS_IOC_BALANCE_CTL, BTRFS_BALANCE_CTL_CANCEL);
	if (ret < 0) {
		error("balance cancel on '%s' failed: %s", path,
			(errno == ENOTCONN) ? "Not in progress" : strerror(errno));
		if (errno == ENOTCONN)
			ret = 2;
		else
			ret = 1;
	}

	btrfs_warn_multiple_profiles(fd);
	close_file_or_dir(fd, dirstream);
	return ret;
}
static DEFINE_SIMPLE_COMMAND(balance_cancel, "cancel");

static const char * const cmd_balance_resume_usage[] = {
	"btrfs balance resume <path>",
	"Resume interrupted balance",
	HELPINFO_INSERT_GLOBALS,
	HELPINFO_INSERT_QUIET,
	NULL
};

static int cmd_balance_resume(const struct cmd_struct *cmd,
			      int argc, char **argv)
{
	struct btrfs_ioctl_balance_args args;
	const char *path;
	DIR *dirstream = NULL;
	int fd;
	int ret;

	clean_args_no_options(cmd, argc, argv);

	if (check_argc_exact(argc - optind, 1))
		return 1;

	path = argv[optind];

	fd = btrfs_open_dir(path, &dirstream, 1);
	if (fd < 0)
		return 1;

	memset(&args, 0, sizeof(args));
	args.flags |= BTRFS_BALANCE_RESUME;

	ret = ioctl(fd, BTRFS_IOC_BALANCE_V2, &args);
	if (ret < 0) {
		if (errno == ECANCELED) {
			if (args.state & BTRFS_BALANCE_STATE_PAUSE_REQ)
				fprintf(stderr, "balance paused by user\n");
			if (args.state & BTRFS_BALANCE_STATE_CANCEL_REQ)
				fprintf(stderr, "balance canceled by user\n");
		} else if (errno == ENOTCONN || errno == EINPROGRESS) {
			error("balance resume on '%s' failed: %s", path,
				(errno == ENOTCONN) ? "Not in progress" :
						  "Already running");
			if (errno == ENOTCONN)
				ret = 2;
			else
				ret = 1;
		} else {
			error("error during balancing '%s': %m\n"
			  "There may be more info in syslog - try dmesg | tail",
				path);
			ret = 1;
		}
	} else {
		pr_verbose(MUST_LOG,
			   "Done, had to relocate %llu out of %llu chunks\n",
			   (unsigned long long)args.stat.completed,
			   (unsigned long long)args.stat.considered);
	}

	close_file_or_dir(fd, dirstream);
	return ret;
}
static DEFINE_SIMPLE_COMMAND(balance_resume, "resume");

static const char * const cmd_balance_status_usage[] = {
	"btrfs balance status [-v] <path>",
	"Show status of running or paused balance",
	"",
	"-v|--verbose     deprecated, alias for global -v option",
	HELPINFO_INSERT_GLOBALS,
	HELPINFO_INSERT_VERBOSE,
	NULL
};

/* Checks the status of the balance if any
 * return codes:
 *   2 : Error failed to know if there is any pending balance
 *   1 : Successful to know status of a pending balance
 *   0 : When there is no pending balance or completed
 */
static int cmd_balance_status(const struct cmd_struct *cmd,
			      int argc, char **argv)
{
	struct btrfs_ioctl_balance_args args;
	const char *path;
	DIR *dirstream = NULL;
	int fd;
	int ret;

	optind = 0;
	while (1) {
		int opt;
		static const struct option longopts[] = {
			{ "verbose", no_argument, NULL, 'v' },
			{ NULL, 0, NULL, 0 }
		};

		opt = getopt_long(argc, argv, "v", longopts, NULL);
		if (opt < 0)
			break;

		switch (opt) {
		case 'v':
			bconf_be_verbose();
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
		return 2;

	ret = ioctl(fd, BTRFS_IOC_BALANCE_PROGRESS, &args);
	if (ret < 0) {
		if (errno == ENOTCONN) {
			printf("No balance found on '%s'\n", path);
			ret = 0;
			goto out;
		}
		error("balance status on '%s' failed: %m", path);
		ret = 2;
		goto out;
	}

	if (args.state & BTRFS_BALANCE_STATE_RUNNING) {
		printf("Balance on '%s' is running", path);
		if (args.state & BTRFS_BALANCE_STATE_CANCEL_REQ)
			printf(", cancel requested\n");
		else if (args.state & BTRFS_BALANCE_STATE_PAUSE_REQ)
			printf(", pause requested\n");
		else
			printf("\n");
	} else {
		printf("Balance on '%s' is paused\n", path);
	}

	printf("%llu out of about %llu chunks balanced (%llu considered), "
	       "%3.f%% left\n", (unsigned long long)args.stat.completed,
	       (unsigned long long)args.stat.expected,
	       (unsigned long long)args.stat.considered,
	       100 * (1 - (float)args.stat.completed/args.stat.expected));

	if (bconf.verbose > BTRFS_BCONF_QUIET)
		dump_ioctl_balance_args(&args);

	ret = 1;
out:
	close_file_or_dir(fd, dirstream);
	return ret;
}
static DEFINE_SIMPLE_COMMAND(balance_status, "status");

static int cmd_balance_full(const struct cmd_struct *cmd, int argc, char **argv)
{
	struct btrfs_ioctl_balance_args args;

	memset(&args, 0, sizeof(args));
	args.flags |= BTRFS_BALANCE_TYPE_MASK;

	/* No enqueueing supported for the obsolete syntax */
	return do_balance(argv[1], &args, BALANCE_START_NOWARN, false);
}
static DEFINE_COMMAND(balance_full, "--full-balance", cmd_balance_full,
		      NULL, NULL, CMD_HIDDEN);

static const char balance_cmd_group_info[] =
"balance data across devices, or change block groups using filters";

static const struct cmd_group balance_cmd_group = {
	balance_cmd_group_usage, balance_cmd_group_info, {
		&cmd_struct_balance_start,
		&cmd_struct_balance_pause,
		&cmd_struct_balance_cancel,
		&cmd_struct_balance_resume,
		&cmd_struct_balance_status,
		&cmd_struct_balance_full,
		NULL
	}
};

static int cmd_balance(const struct cmd_struct *cmd, int argc, char **argv)
{
	if (argc == 2 && strcmp("start", argv[1]) != 0) {
		/* old 'btrfs filesystem balance <path>' syntax */
		struct btrfs_ioctl_balance_args args;

		memset(&args, 0, sizeof(args));
		args.flags |= BTRFS_BALANCE_TYPE_MASK;

		/* No enqueueing supported for the obsolete syntax */
		return do_balance(argv[1], &args, 0, false);
	}

	return handle_command_group(cmd, argc, argv);
}

DEFINE_COMMAND(balance, "balance", cmd_balance, NULL, &balance_cmd_group, 0);
