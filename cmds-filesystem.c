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
#include <sys/ioctl.h>
#include <errno.h>
#include <uuid/uuid.h>
#include <ctype.h>

#include "kerncompat.h"
#include "ctree.h"
#include "ioctl.h"
#include "utils.h"
#include "volumes.h"

#include "version.h"

#include "commands.h"
#include "btrfslabel.h"

static const char * const filesystem_cmd_group_usage[] = {
	"btrfs filesystem [<group>] <command> [<args>]",
	NULL
};

static const char * const cmd_df_usage[] = {
	"btrfs filesystem df <path>",
	"Show space usage information for a mount point",
	NULL
};

static int cmd_df(int argc, char **argv)
{
	struct btrfs_ioctl_space_args *sargs, *sargs_orig;
	u64 count = 0, i;
	int ret;
	int fd;
	int e;
	char *path;

	if (check_argc_exact(argc, 2))
		usage(cmd_df_usage);

	path = argv[1];

	fd = open_file_or_dir(path);
	if (fd < 0) {
		fprintf(stderr, "ERROR: can't access to '%s'\n", path);
		return 12;
	}

	sargs_orig = sargs = malloc(sizeof(struct btrfs_ioctl_space_args));
	if (!sargs)
		return -ENOMEM;

	sargs->space_slots = 0;
	sargs->total_spaces = 0;

	ret = ioctl(fd, BTRFS_IOC_SPACE_INFO, sargs);
	e = errno;
	if (ret) {
		fprintf(stderr, "ERROR: couldn't get space info on '%s' - %s\n",
			path, strerror(e));
		close(fd);
		free(sargs);
		return ret;
	}
	if (!sargs->total_spaces) {
		close(fd);
		free(sargs);
		return 0;
	}

	count = sargs->total_spaces;

	sargs = realloc(sargs, sizeof(struct btrfs_ioctl_space_args) +
			(count * sizeof(struct btrfs_ioctl_space_info)));
	if (!sargs) {
		close(fd);
		free(sargs_orig);
		return -ENOMEM;
	}

	sargs->space_slots = count;
	sargs->total_spaces = 0;

	ret = ioctl(fd, BTRFS_IOC_SPACE_INFO, sargs);
	e = errno;
	if (ret) {
		fprintf(stderr, "ERROR: couldn't get space info on '%s' - %s\n",
			path, strerror(e));
		close(fd);
		free(sargs);
		return ret;
	}

	for (i = 0; i < sargs->total_spaces; i++) {
		char description[80];
		char *total_bytes;
		char *used_bytes;
		int written = 0;
		u64 flags = sargs->spaces[i].flags;

		memset(description, 0, 80);

		if (flags & BTRFS_BLOCK_GROUP_DATA) {
			if (flags & BTRFS_BLOCK_GROUP_METADATA) {
				snprintf(description, 14, "%s",
					 "Data+Metadata");
				written += 13;
			} else {
				snprintf(description, 5, "%s", "Data");
				written += 4;
			}
		} else if (flags & BTRFS_BLOCK_GROUP_SYSTEM) {
			snprintf(description, 7, "%s", "System");
			written += 6;
		} else if (flags & BTRFS_BLOCK_GROUP_METADATA) {
			snprintf(description, 9, "%s", "Metadata");
			written += 8;
		}

		if (flags & BTRFS_BLOCK_GROUP_RAID0) {
			snprintf(description+written, 8, "%s", ", RAID0");
			written += 7;
		} else if (flags & BTRFS_BLOCK_GROUP_RAID1) {
			snprintf(description+written, 8, "%s", ", RAID1");
			written += 7;
		} else if (flags & BTRFS_BLOCK_GROUP_DUP) {
			snprintf(description+written, 6, "%s", ", DUP");
			written += 5;
		} else if (flags & BTRFS_BLOCK_GROUP_RAID10) {
			snprintf(description+written, 9, "%s", ", RAID10");
			written += 8;
		}

		total_bytes = pretty_sizes(sargs->spaces[i].total_bytes);
		used_bytes = pretty_sizes(sargs->spaces[i].used_bytes);
		printf("%s: total=%s, used=%s\n", description, total_bytes,
		       used_bytes);
	}
	close(fd);
	free(sargs);

	return 0;
}

static int uuid_search(struct btrfs_fs_devices *fs_devices, char *search)
{
	char uuidbuf[37];
	struct list_head *cur;
	struct btrfs_device *device;
	int search_len = strlen(search);

	search_len = min(search_len, 37);
	uuid_unparse(fs_devices->fsid, uuidbuf);
	if (!strncmp(uuidbuf, search, search_len))
		return 1;

	list_for_each(cur, &fs_devices->devices) {
		device = list_entry(cur, struct btrfs_device, dev_list);
		if ((device->label && strcmp(device->label, search) == 0) ||
		    strcmp(device->name, search) == 0)
			return 1;
	}
	return 0;
}

static void print_one_uuid(struct btrfs_fs_devices *fs_devices)
{
	char uuidbuf[37];
	struct list_head *cur;
	struct btrfs_device *device;
	char *super_bytes_used;
	u64 devs_found = 0;
	u64 total;

	uuid_unparse(fs_devices->fsid, uuidbuf);
	device = list_entry(fs_devices->devices.next, struct btrfs_device,
			    dev_list);
	if (device->label && device->label[0])
		printf("Label: '%s' ", device->label);
	else
		printf("Label: none ");

	super_bytes_used = pretty_sizes(device->super_bytes_used);

	total = device->total_devs;
	printf(" uuid: %s\n\tTotal devices %llu FS bytes used %s\n", uuidbuf,
	       (unsigned long long)total, super_bytes_used);

	free(super_bytes_used);

	list_for_each(cur, &fs_devices->devices) {
		char *total_bytes;
		char *bytes_used;
		device = list_entry(cur, struct btrfs_device, dev_list);
		total_bytes = pretty_sizes(device->total_bytes);
		bytes_used = pretty_sizes(device->bytes_used);
		printf("\tdevid %4llu size %s used %s path %s\n",
		       (unsigned long long)device->devid,
		       total_bytes, bytes_used, device->name);
		free(total_bytes);
		free(bytes_used);
		devs_found++;
	}
	if (devs_found < total) {
		printf("\t*** Some devices missing\n");
	}
	printf("\n");
}

static const char * const cmd_show_usage[] = {
	"btrfs filesystem show [--all-devices] [<uuid>|<label>]",
	"Show the structure of a filesystem",
	"If no argument is given, structure of all present filesystems is shown.",
	NULL
};

static int cmd_show(int argc, char **argv)
{
	struct list_head *all_uuids;
	struct btrfs_fs_devices *fs_devices;
	struct list_head *cur_uuid;
	char *search = 0;
	int ret;
	int checklist = 1;
	int searchstart = 1;

	if( argc > 1 && !strcmp(argv[1],"--all-devices")){
		checklist = 0;
		searchstart += 1;
	}

	if (check_argc_max(argc, searchstart + 1))
		usage(cmd_show_usage);

	if(checklist)
		ret = btrfs_scan_block_devices(0);
	else
		ret = btrfs_scan_one_dir("/dev", 0);

	if (ret){
		fprintf(stderr, "ERROR: error %d while scanning\n", ret);
		return 18;
	}
	
	if(searchstart < argc)
		search = argv[searchstart];

	all_uuids = btrfs_scanned_uuids();
	list_for_each(cur_uuid, all_uuids) {
		fs_devices = list_entry(cur_uuid, struct btrfs_fs_devices,
					list);
		if (search && uuid_search(fs_devices, search) == 0)
			continue;
		print_one_uuid(fs_devices);
	}
	printf("%s\n", BTRFS_BUILD_VERSION);
	return 0;
}

static const char * const cmd_sync_usage[] = {
	"btrfs filesystem sync <path>",
	"Force a sync on a filesystem",
	NULL
};

static int cmd_sync(int argc, char **argv)
{
	int 	fd, res, e;
	char	*path;

	if (check_argc_exact(argc, 2))
		usage(cmd_sync_usage);

	path = argv[1];

	fd = open_file_or_dir(path);
	if (fd < 0) {
		fprintf(stderr, "ERROR: can't access to '%s'\n", path);
		return 12;
	}

	printf("FSSync '%s'\n", path);
	res = ioctl(fd, BTRFS_IOC_SYNC);
	e = errno;
	close(fd);
	if( res < 0 ){
		fprintf(stderr, "ERROR: unable to fs-syncing '%s' - %s\n", 
			path, strerror(e));
		return 16;
	}

	return 0;
}

static u64 parse_size(char *s)
{
	int len = strlen(s);
	char c;
	u64 mult = 1;

	if (!isdigit(s[len - 1])) {
		c = tolower(s[len - 1]);
		switch (c) {
		case 'g':
			mult *= 1024;
		case 'm':
			mult *= 1024;
		case 'k':
			mult *= 1024;
		case 'b':
			break;
		default:
			fprintf(stderr, "Unknown size descriptor %c\n", c);
			exit(1);
		}
		s[len - 1] = '\0';
	}
	return atoll(s) * mult;
}

static int parse_compress_type(char *s)
{
	if (strcmp(optarg, "zlib") == 0)
		return BTRFS_COMPRESS_ZLIB;
	else if (strcmp(optarg, "lzo") == 0)
		return BTRFS_COMPRESS_LZO;
	else {
		fprintf(stderr, "Unknown compress type %s\n", s);
		exit(1);
	};
}

static const char * const cmd_defrag_usage[] = {
	"btrfs filesystem defragment [options] <file>|<dir> [<file>|<dir>...]",
	"Defragment a file or a directory",
	"",
	"-v             be verbose",
	"-c[zlib,lzo]   compress the file while defragmenting",
	"-f             flush data to disk immediately after defragmenting",
	"-s start       defragment only from byte onward",
	"-l len         defragment only up to len bytes",
	"-t size        minimal size of file to be considered for defragmenting",
	NULL
};

static int cmd_defrag(int argc, char **argv)
{
	int fd;
	int flush = 0;
	u64 start = 0;
	u64 len = (u64)-1;
	u32 thresh = 0;
	int i;
	int errors = 0;
	int ret = 0;
	int verbose = 0;
	int fancy_ioctl = 0;
	struct btrfs_ioctl_defrag_range_args range;
	int e=0;
	int compress_type = BTRFS_COMPRESS_NONE;

	optind = 1;
	while(1) {
		int c = getopt(argc, argv, "vc::fs:l:t:");
		if (c < 0)
			break;

		switch(c) {
		case 'c':
			compress_type = BTRFS_COMPRESS_ZLIB;
			if (optarg)
				compress_type = parse_compress_type(optarg);
			fancy_ioctl = 1;
			break;
		case 'f':
			flush = 1;
			fancy_ioctl = 1;
			break;
		case 'v':
			verbose = 1;
			break;
		case 's':
			start = parse_size(optarg);
			fancy_ioctl = 1;
			break;
		case 'l':
			len = parse_size(optarg);
			fancy_ioctl = 1;
			break;
		case 't':
			thresh = parse_size(optarg);
			fancy_ioctl = 1;
			break;
		default:
			usage(cmd_defrag_usage);
		}
	}

	if (check_argc_min(argc - optind, 1))
		usage(cmd_defrag_usage);

	memset(&range, 0, sizeof(range));
	range.start = start;
	range.len = len;
	range.extent_thresh = thresh;
	if (compress_type) {
		range.flags |= BTRFS_DEFRAG_RANGE_COMPRESS;
		range.compress_type = compress_type;
	}
	if (flush)
		range.flags |= BTRFS_DEFRAG_RANGE_START_IO;

	for (i = optind; i < argc; i++) {
		if (verbose)
			printf("%s\n", argv[i]);
		fd = open_file_or_dir(argv[i]);
		if (fd < 0) {
			fprintf(stderr, "failed to open %s\n", argv[i]);
			perror("open:");
			errors++;
			continue;
		}
		if (!fancy_ioctl) {
			ret = ioctl(fd, BTRFS_IOC_DEFRAG, NULL);
			e=errno;
		} else {
			ret = ioctl(fd, BTRFS_IOC_DEFRAG_RANGE, &range);
			if (ret && errno == ENOTTY) {
				fprintf(stderr, "ERROR: defrag range ioctl not "
					"supported in this kernel, please try "
					"without any options.\n");
				errors++;
				close(fd);
				break;
			}
		}
		if (ret) {
			fprintf(stderr, "ERROR: defrag failed on %s - %s\n",
				argv[i], strerror(e));
			errors++;
		}
		close(fd);
	}
	if (verbose)
		printf("%s\n", BTRFS_BUILD_VERSION);
	if (errors) {
		fprintf(stderr, "total %d failures\n", errors);
		exit(1);
	}

	return errors + 20;
}

static const char * const cmd_resize_usage[] = {
	"btrfs filesystem resize [devid:][+/-]<newsize>[gkm]|[devid:]max <path>",
	"Resize a filesystem",
	"If 'max' is passed, the filesystem will occupy all available space",
	"on the device 'devid'.",
	NULL
};

static int cmd_resize(int argc, char **argv)
{
	struct btrfs_ioctl_vol_args	args;
	int	fd, res, len, e;
	char	*amount, *path;

	if (check_argc_exact(argc, 3))
		usage(cmd_resize_usage);

	amount = argv[1];
	path = argv[2];

	fd = open_file_or_dir(path);
	if (fd < 0) {
		fprintf(stderr, "ERROR: can't access to '%s'\n", path);
		return 12;
	}
	len = strlen(amount);
	if (len == 0 || len >= BTRFS_VOL_NAME_MAX) {
		fprintf(stderr, "ERROR: size value too long ('%s)\n",
			amount);
		return 14;
	}

	printf("Resize '%s' of '%s'\n", path, amount);
	strncpy(args.name, amount, BTRFS_PATH_NAME_MAX);
	args.name[BTRFS_PATH_NAME_MAX-1] = 0;
	res = ioctl(fd, BTRFS_IOC_RESIZE, &args);
	e = errno;
	close(fd);
	if( res < 0 ){
		fprintf(stderr, "ERROR: unable to resize '%s' - %s\n", 
			path, strerror(e));
		return 30;
	}
	return 0;
}

static const char * const cmd_label_usage[] = {
	"btrfs filesystem label <device> [<newlabel>]",
	"Get or change the label of an unmounted filesystem",
	"With one argument, get the label of filesystem on <device>.",
	"If <newlabel> is passed, set the filesystem label to <newlabel>.",
	NULL
};

static int cmd_label(int argc, char **argv)
{
	if (check_argc_min(argc, 2) || check_argc_max(argc, 3))
		usage(cmd_label_usage);

	if (argc > 2)
		return set_label(argv[1], argv[2]);
	else
		return get_label(argv[1]);
}

const struct cmd_group filesystem_cmd_group = {
	filesystem_cmd_group_usage, NULL, {
		{ "df", cmd_df, cmd_df_usage, NULL, 0 },
		{ "show", cmd_show, cmd_show_usage, NULL, 0 },
		{ "sync", cmd_sync, cmd_sync_usage, NULL, 0 },
		{ "defragment", cmd_defrag, cmd_defrag_usage, NULL, 0 },
		{ "balance", cmd_balance, NULL, &balance_cmd_group, 1 },
		{ "resize", cmd_resize, cmd_resize_usage, NULL, 0 },
		{ "label", cmd_label, cmd_label_usage, NULL, 0 },
		{ 0, 0, 0, 0, 0 },
	}
};

int cmd_filesystem(int argc, char **argv)
{
	return handle_command_group(&filesystem_cmd_group, argc, argv);
}
