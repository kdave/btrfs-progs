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
#include <errno.h>

#include "kerncompat.h"
#include "ctree.h"
#include "ioctl.h"
#include "volumes.h"

#include "commands.h"
#include "utils.h"

static const char * const balance_cmd_group_usage[] = {
	"btrfs [filesystem] balance <command> [options] <path>",
	"btrfs [filesystem] balance <path>",
	NULL
};

static const char balance_cmd_group_info[] =
	"'btrfs filesystem balance' command is deprecated, please use\n"
	"'btrfs balance start' command instead.";

static int parse_one_profile(const char *profile, u64 *flags)
{
	if (!strcmp(profile, "raid0")) {
		*flags |= BTRFS_BLOCK_GROUP_RAID0;
	} else if (!strcmp(profile, "raid1")) {
		*flags |= BTRFS_BLOCK_GROUP_RAID1;
	} else if (!strcmp(profile, "raid10")) {
		*flags |= BTRFS_BLOCK_GROUP_RAID10;
	} else if (!strcmp(profile, "raid5")) {
		*flags |= BTRFS_BLOCK_GROUP_RAID5;
	} else if (!strcmp(profile, "raid6")) {
		*flags |= BTRFS_BLOCK_GROUP_RAID6;
	} else if (!strcmp(profile, "dup")) {
		*flags |= BTRFS_BLOCK_GROUP_DUP;
	} else if (!strcmp(profile, "single")) {
		*flags |= BTRFS_AVAIL_ALLOC_BIT_SINGLE;
	} else {
		fprintf(stderr, "Unknown profile '%s'\n", profile);
		return 1;
	}

	return 0;
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

static int parse_u64(const char *str, u64 *result)
{
	char *endptr;
	u64 val;

	val = strtoull(str, &endptr, 10);
	if (*endptr)
		return 1;

	*result = val;
	return 0;
}

static int parse_range(const char *range, u64 *start, u64 *end)
{
	char *dots;

	dots = strstr(range, "..");
	if (dots) {
		const char *rest = dots + 2;
		int skipped = 0;

		*dots = 0;

		if (!*rest) {
			*end = (u64)-1;
			skipped++;
		} else {
			if (parse_u64(rest, end))
				return 1;
		}
		if (dots == range) {
			*start = 0;
			skipped++;
		} else {
			if (parse_u64(range, start))
				return 1;
		}

		if (*start >= *end) {
			fprintf(stderr, "Range %llu..%llu doesn't make "
				"sense\n", (unsigned long long)*start,
				(unsigned long long)*end);
			return 1;
		}

		if (skipped <= 1)
			return 0;
	}

	return 1;
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
				fprintf(stderr, "the profiles filter requires "
				       "an argument\n");
				return 1;
			}
			if (parse_profiles(value, &args->profiles)) {
				fprintf(stderr, "Invalid profiles argument\n");
				return 1;
			}
			args->flags |= BTRFS_BALANCE_ARGS_PROFILES;
		} else if (!strcmp(this_char, "usage")) {
			if (!value || !*value) {
				fprintf(stderr, "the usage filter requires "
				       "an argument\n");
				return 1;
			}
			if (parse_u64(value, &args->usage) ||
			    args->usage > 100) {
				fprintf(stderr, "Invalid usage argument: %s\n",
				       value);
				return 1;
			}
			args->flags |= BTRFS_BALANCE_ARGS_USAGE;
		} else if (!strcmp(this_char, "devid")) {
			if (!value || !*value) {
				fprintf(stderr, "the devid filter requires "
				       "an argument\n");
				return 1;
			}
			if (parse_u64(value, &args->devid) ||
			    args->devid == 0) {
				fprintf(stderr, "Invalid devid argument: %s\n",
				       value);
				return 1;
			}
			args->flags |= BTRFS_BALANCE_ARGS_DEVID;
		} else if (!strcmp(this_char, "drange")) {
			if (!value || !*value) {
				fprintf(stderr, "the drange filter requires "
				       "an argument\n");
				return 1;
			}
			if (parse_range(value, &args->pstart, &args->pend)) {
				fprintf(stderr, "Invalid drange argument\n");
				return 1;
			}
			args->flags |= BTRFS_BALANCE_ARGS_DRANGE;
		} else if (!strcmp(this_char, "vrange")) {
			if (!value || !*value) {
				fprintf(stderr, "the vrange filter requires "
				       "an argument\n");
				return 1;
			}
			if (parse_range(value, &args->vstart, &args->vend)) {
				fprintf(stderr, "Invalid vrange argument\n");
				return 1;
			}
			args->flags |= BTRFS_BALANCE_ARGS_VRANGE;
		} else if (!strcmp(this_char, "convert")) {
			if (!value || !*value) {
				fprintf(stderr, "the convert option requires "
				       "an argument\n");
				return 1;
			}
			if (parse_one_profile(value, &args->target)) {
				fprintf(stderr, "Invalid convert argument\n");
				return 1;
			}
			args->flags |= BTRFS_BALANCE_ARGS_CONVERT;
		} else if (!strcmp(this_char, "soft")) {
			args->flags |= BTRFS_BALANCE_ARGS_SOFT;
		} else if (!strcmp(this_char, "limit")) {
			if (!value || !*value) {
				fprintf(stderr,
					"the limit filter requires an argument\n");
				return 1;
			}
			if (parse_u64(value, &args->limit)) {
				fprintf(stderr, "Invalid limit argument: %s\n",
				       value);
				return 1;
			}
			args->flags |= BTRFS_BALANCE_ARGS_LIMIT;
		} else {
			fprintf(stderr, "Unrecognized balance option '%s'\n",
				this_char);
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

static int do_balance(const char *path, struct btrfs_ioctl_balance_args *args,
		      int nofilters)
{
	int fd;
	int ret;
	int e;
	DIR *dirstream = NULL;

	fd = open_file_or_dir(path, &dirstream);
	if (fd < 0) {
		fprintf(stderr, "ERROR: can't access '%s'\n", path);
		return 1;
	}

	ret = ioctl(fd, BTRFS_IOC_BALANCE_V2, args);
	e = errno;

	if (ret < 0) {
		/*
		 * older kernels don't have the new balance ioctl, try the
		 * old one.  But, the old one doesn't know any filters, so
		 * don't fall back if they tried to use the fancy new things
		 */
		if (e == ENOTTY && nofilters) {
			ret = do_balance_v1(fd);
			if (ret == 0)
				goto out;
			e = errno;
		}

		if (e == ECANCELED) {
			if (args->state & BTRFS_BALANCE_STATE_PAUSE_REQ)
				fprintf(stderr, "balance paused by user\n");
			if (args->state & BTRFS_BALANCE_STATE_CANCEL_REQ)
				fprintf(stderr, "balance canceled by user\n");
			ret = 0;
		} else {
			fprintf(stderr, "ERROR: error during balancing '%s' "
				"- %s\n", path, strerror(e));
			if (e != EINPROGRESS)
				fprintf(stderr, "There may be more info in "
					"syslog - try dmesg | tail\n");
			ret = 1;
		}
	} else {
		printf("Done, had to relocate %llu out of %llu chunks\n",
		       (unsigned long long)args->stat.completed,
		       (unsigned long long)args->stat.considered);
		ret = 0;
	}

out:
	close_file_or_dir(fd, dirstream);
	return ret;
}

static const char * const cmd_balance_start_usage[] = {
	"btrfs [filesystem] balance start [options] <path>",
	"Balance chunks across the devices",
	"Balance and/or convert (change allocation profile of) chunks that",
	"passed all filters in a comma-separated list of filters for a",
	"particular chunk type.  If filter list is not given balance all",
	"chunks of that type.  In case none of the -d, -m or -s options is",
	"given balance all chunks in a filesystem.",
	"",
	"-d[filters]    act on data chunks",
	"-m[filters]    act on metadata chunks",
	"-s[filters]    act on system chunks (only under -f)",
	"-v             be verbose",
	"-f             force reducing of metadata integrity",
	NULL
};

static int cmd_balance_start(int argc, char **argv)
{
	struct btrfs_ioctl_balance_args args;
	struct btrfs_balance_args *ptrs[] = { &args.data, &args.sys,
						&args.meta, NULL };
	int force = 0;
	int verbose = 0;
	int nofilters = 1;
	int i;

	memset(&args, 0, sizeof(args));

	optind = 1;
	while (1) {
		int longindex;
		static const struct option longopts[] = {
			{ "data", optional_argument, NULL, 'd'},
			{ "metadata", optional_argument, NULL, 'm' },
			{ "system", optional_argument, NULL, 's' },
			{ "force", no_argument, NULL, 'f' },
			{ "verbose", no_argument, NULL, 'v' },
			{ NULL, 0, NULL, 0 }
		};

		int opt = getopt_long(argc, argv, "d::s::m::fv", longopts,
				      &longindex);
		if (opt < 0)
			break;

		switch (opt) {
		case 'd':
			nofilters = 0;
			args.flags |= BTRFS_BALANCE_DATA;

			if (parse_filters(optarg, &args.data))
				return 1;
			break;
		case 's':
			nofilters = 0;
			args.flags |= BTRFS_BALANCE_SYSTEM;

			if (parse_filters(optarg, &args.sys))
				return 1;
			break;
		case 'm':
			nofilters = 0;
			args.flags |= BTRFS_BALANCE_METADATA;

			if (parse_filters(optarg, &args.meta))
				return 1;
			break;
		case 'f':
			force = 1;
			break;
		case 'v':
			verbose = 1;
			break;
		default:
			usage(cmd_balance_start_usage);
		}
	}

	if (check_argc_exact(argc - optind, 1))
		usage(cmd_balance_start_usage);

	/*
	 * allow -s only under --force, otherwise do with system chunks
	 * the same thing we were ordered to do with meta chunks
	 */
	if (args.flags & BTRFS_BALANCE_SYSTEM) {
		if (!force) {
			fprintf(stderr,
"Refusing to explicitly operate on system chunks.\n"
"Pass --force if you really want to do that.\n");
			return 1;
		}
	} else if (args.flags & BTRFS_BALANCE_METADATA) {
		args.flags |= BTRFS_BALANCE_SYSTEM;
		memcpy(&args.sys, &args.meta,
			sizeof(struct btrfs_balance_args));
	}

	if (nofilters) {
		/* relocate everything - no filters */
		args.flags |= BTRFS_BALANCE_TYPE_MASK;
	}

	/* drange makes sense only when devid is set */
	for (i = 0; ptrs[i]; i++) {
		if ((ptrs[i]->flags & BTRFS_BALANCE_ARGS_DRANGE) &&
		    !(ptrs[i]->flags & BTRFS_BALANCE_ARGS_DEVID)) {
			fprintf(stderr, "drange filter can be used only if "
				"devid filter is used\n");
			return 1;
		}
	}

	/* soft makes sense only when convert for corresponding type is set */
	for (i = 0; ptrs[i]; i++) {
		if ((ptrs[i]->flags & BTRFS_BALANCE_ARGS_SOFT) &&
		    !(ptrs[i]->flags & BTRFS_BALANCE_ARGS_CONVERT)) {
			fprintf(stderr, "'soft' option can be used only if "
				"changing profiles\n");
			return 1;
		}
	}

	if (force)
		args.flags |= BTRFS_BALANCE_FORCE;
	if (verbose)
		dump_ioctl_balance_args(&args);

	return do_balance(argv[optind], &args, nofilters);
}

static const char * const cmd_balance_pause_usage[] = {
	"btrfs [filesystem] balance pause <path>",
	"Pause running balance",
	NULL
};

static int cmd_balance_pause(int argc, char **argv)
{
	const char *path;
	int fd;
	int ret;
	int e;
	DIR *dirstream = NULL;

	if (check_argc_exact(argc, 2))
		usage(cmd_balance_pause_usage);

	path = argv[1];

	fd = open_file_or_dir(path, &dirstream);
	if (fd < 0) {
		fprintf(stderr, "ERROR: can't access '%s'\n", path);
		return 1;
	}

	ret = ioctl(fd, BTRFS_IOC_BALANCE_CTL, BTRFS_BALANCE_CTL_PAUSE);
	e = errno;
	close_file_or_dir(fd, dirstream);

	if (ret < 0) {
		fprintf(stderr, "ERROR: balance pause on '%s' failed - %s\n",
			path, (e == ENOTCONN) ? "Not running" : strerror(e));
		if (e == ENOTCONN)
			return 2;
		else
			return 1;
	}

	return 0;
}

static const char * const cmd_balance_cancel_usage[] = {
	"btrfs [filesystem] balance cancel <path>",
	"Cancel running or paused balance",
	NULL
};

static int cmd_balance_cancel(int argc, char **argv)
{
	const char *path;
	int fd;
	int ret;
	int e;
	DIR *dirstream = NULL;

	if (check_argc_exact(argc, 2))
		usage(cmd_balance_cancel_usage);

	path = argv[1];

	fd = open_file_or_dir(path, &dirstream);
	if (fd < 0) {
		fprintf(stderr, "ERROR: can't access '%s'\n", path);
		return 1;
	}

	ret = ioctl(fd, BTRFS_IOC_BALANCE_CTL, BTRFS_BALANCE_CTL_CANCEL);
	e = errno;
	close_file_or_dir(fd, dirstream);

	if (ret < 0) {
		fprintf(stderr, "ERROR: balance cancel on '%s' failed - %s\n",
			path, (e == ENOTCONN) ? "Not in progress" : strerror(e));
		if (e == ENOTCONN)
			return 2;
		else
			return 1;
	}

	return 0;
}

static const char * const cmd_balance_resume_usage[] = {
	"btrfs [filesystem] balance resume <path>",
	"Resume interrupted balance",
	NULL
};

static int cmd_balance_resume(int argc, char **argv)
{
	struct btrfs_ioctl_balance_args args;
	const char *path;
	DIR *dirstream = NULL;
	int fd;
	int ret;
	int e;

	if (check_argc_exact(argc, 2))
		usage(cmd_balance_resume_usage);

	path = argv[1];

	fd = open_file_or_dir(path, &dirstream);
	if (fd < 0) {
		fprintf(stderr, "ERROR: can't access '%s'\n", path);
		return 1;
	}

	memset(&args, 0, sizeof(args));
	args.flags |= BTRFS_BALANCE_RESUME;

	ret = ioctl(fd, BTRFS_IOC_BALANCE_V2, &args);
	e = errno;
	close_file_or_dir(fd, dirstream);

	if (ret < 0) {
		if (e == ECANCELED) {
			if (args.state & BTRFS_BALANCE_STATE_PAUSE_REQ)
				fprintf(stderr, "balance paused by user\n");
			if (args.state & BTRFS_BALANCE_STATE_CANCEL_REQ)
				fprintf(stderr, "balance canceled by user\n");
		} else if (e == ENOTCONN || e == EINPROGRESS) {
			fprintf(stderr, "ERROR: balance resume on '%s' "
				"failed - %s\n", path,
				(e == ENOTCONN) ? "Not in progress" :
						  "Already running");
			if (e == ENOTCONN)
				return 2;
			else
				return 1;
		} else {
			fprintf(stderr,
"ERROR: error during balancing '%s' - %s\n"
"There may be more info in syslog - try dmesg | tail\n", path, strerror(e));
			return 1;
		}
	} else {
		printf("Done, had to relocate %llu out of %llu chunks\n",
		       (unsigned long long)args.stat.completed,
		       (unsigned long long)args.stat.considered);
	}

	return 0;
}

static const char * const cmd_balance_status_usage[] = {
	"btrfs [filesystem] balance status [-v] <path>",
	"Show status of running or paused balance",
	"",
	"-v     be verbose",
	NULL
};

/* Checks the status of the balance if any
 * return codes:
 *   2 : Error failed to know if there is any pending balance
 *   1 : Successful to know status of a pending balance
 *   0 : When there is no pending balance or completed
 */
static int cmd_balance_status(int argc, char **argv)
{
	struct btrfs_ioctl_balance_args args;
	const char *path;
	DIR *dirstream = NULL;
	int fd;
	int verbose = 0;
	int ret;
	int e;

	optind = 1;
	while (1) {
		int longindex;
		static const struct option longopts[] = {
			{ "verbose", no_argument, NULL, 'v' },
			{ NULL, 0, NULL, 0 }
		};

		int opt = getopt_long(argc, argv, "v", longopts, &longindex);
		if (opt < 0)
			break;

		switch (opt) {
		case 'v':
			verbose = 1;
			break;
		default:
			usage(cmd_balance_status_usage);
		}
	}

	if (check_argc_exact(argc - optind, 1))
		usage(cmd_balance_status_usage);

	path = argv[optind];

	fd = open_file_or_dir(path, &dirstream);
	if (fd < 0) {
		fprintf(stderr, "ERROR: can't access '%s'\n", path);
		return 2;
	}

	ret = ioctl(fd, BTRFS_IOC_BALANCE_PROGRESS, &args);
	e = errno;
	close_file_or_dir(fd, dirstream);

	if (ret < 0) {
		if (e == ENOTCONN) {
			printf("No balance found on '%s'\n", path);
			return 0;
		}
		fprintf(stderr, "ERROR: balance status on '%s' failed - %s\n",
			path, strerror(e));
		return 2;
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

	if (verbose)
		dump_ioctl_balance_args(&args);

	return 1;
}

const struct cmd_group balance_cmd_group = {
	balance_cmd_group_usage, balance_cmd_group_info, {
		{ "start", cmd_balance_start, cmd_balance_start_usage, NULL, 0 },
		{ "pause", cmd_balance_pause, cmd_balance_pause_usage, NULL, 0 },
		{ "cancel", cmd_balance_cancel, cmd_balance_cancel_usage, NULL, 0 },
		{ "resume", cmd_balance_resume, cmd_balance_resume_usage, NULL, 0 },
		{ "status", cmd_balance_status, cmd_balance_status_usage, NULL, 0 },
		NULL_CMD_STRUCT
	}
};

int cmd_balance(int argc, char **argv)
{
	if (argc == 2) {
		/* old 'btrfs filesystem balance <path>' syntax */
		struct btrfs_ioctl_balance_args args;

		memset(&args, 0, sizeof(args));
		args.flags |= BTRFS_BALANCE_TYPE_MASK;

		return do_balance(argv[1], &args, 1);
	}

	return handle_command_group(&balance_cmd_group, argc, argv);
}
