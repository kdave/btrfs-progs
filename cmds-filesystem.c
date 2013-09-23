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

#define _XOPEN_SOURCE 500
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/ioctl.h>
#include <errno.h>
#include <uuid/uuid.h>
#include <ctype.h>
#include <fcntl.h>
#include <ftw.h>

#include "kerncompat.h"
#include "ctree.h"
#include "ioctl.h"
#include "utils.h"
#include "volumes.h"
#include "version.h"
#include "commands.h"
#include "list_sort.h"

static const char * const filesystem_cmd_group_usage[] = {
	"btrfs filesystem [<group>] <command> [<args>]",
	NULL
};

static const char * const cmd_df_usage[] = {
	"btrfs filesystem df <path>",
	"Show space usage information for a mount point",
	NULL
};

static char *group_type_str(u64 flag)
{
	switch (flag & BTRFS_BLOCK_GROUP_TYPE_MASK) {
	case BTRFS_BLOCK_GROUP_DATA:
		return "Data";
	case BTRFS_BLOCK_GROUP_SYSTEM:
		return "System";
	case BTRFS_BLOCK_GROUP_METADATA:
		return "Metadata";
	case BTRFS_BLOCK_GROUP_DATA|BTRFS_BLOCK_GROUP_METADATA:
		return "Data+Metadata";
	default:
		return "unknown";
	}
}

static char *group_profile_str(u64 flag)
{
	switch (flag & BTRFS_BLOCK_GROUP_PROFILE_MASK) {
	case 0:
		return "single";
	case BTRFS_BLOCK_GROUP_RAID0:
		return "RAID0";
	case BTRFS_BLOCK_GROUP_RAID1:
		return "RAID1";
	case BTRFS_BLOCK_GROUP_RAID5:
		return "RAID5";
	case BTRFS_BLOCK_GROUP_RAID6:
		return "RAID6";
	case BTRFS_BLOCK_GROUP_DUP:
		return "DUP";
	case BTRFS_BLOCK_GROUP_RAID10:
		return "RAID10";
	default:
		return "unknown";
	}
}

static int get_df(int fd, struct btrfs_ioctl_space_args **sargs_ret)
{
	u64 count = 0;
	int ret, e;
	struct btrfs_ioctl_space_args *sargs;

	sargs = malloc(sizeof(struct btrfs_ioctl_space_args));
	if (!sargs)
		return -ENOMEM;

	sargs->space_slots = 0;
	sargs->total_spaces = 0;

	ret = ioctl(fd, BTRFS_IOC_SPACE_INFO, sargs);
	e = errno;
	if (ret) {
		fprintf(stderr, "ERROR: couldn't get space info - %s\n",
			strerror(e));
		free(sargs);
		return ret;
	}
	if (!sargs->total_spaces) {
		free(sargs);
		return 0;
	}
	count = sargs->total_spaces;
	free(sargs);

	sargs = malloc(sizeof(struct btrfs_ioctl_space_args) +
			(count * sizeof(struct btrfs_ioctl_space_info)));
	if (!sargs)
		ret = -ENOMEM;

	sargs->space_slots = count;
	sargs->total_spaces = 0;
	ret = ioctl(fd, BTRFS_IOC_SPACE_INFO, sargs);
	e = errno;
	if (ret) {
		fprintf(stderr, "ERROR: get space info count %llu - %s\n",
				count, strerror(e));
		free(sargs);
		return ret;
	}
	*sargs_ret = sargs;
	return 0;
}

static void print_df(struct btrfs_ioctl_space_args *sargs)
{
	u64 i;
	struct btrfs_ioctl_space_info *sp = sargs->spaces;

	for (i = 0; i < sargs->total_spaces; i++, sp++) {
		printf("%s, %s: total=%s, used=%s\n",
			group_type_str(sp->flags),
			group_profile_str(sp->flags),
			pretty_size(sp->total_bytes),
			pretty_size(sp->used_bytes));
	}
}

static int cmd_df(int argc, char **argv)
{
	struct btrfs_ioctl_space_args *sargs = NULL;
	int ret;
	int fd;
	char *path;
	DIR  *dirstream = NULL;

	if (check_argc_exact(argc, 2))
		usage(cmd_df_usage);

	path = argv[1];

	fd = open_file_or_dir(path, &dirstream);
	if (fd < 0) {
		fprintf(stderr, "ERROR: can't access to '%s'\n", path);
		return 1;
	}
	ret = get_df(fd, &sargs);

	if (!ret && sargs) {
		print_df(sargs);
		free(sargs);
	} else {
		fprintf(stderr, "ERROR: get_df failed %s\n", strerror(ret));
	}

	close_file_or_dir(fd, dirstream);
	return !!ret;
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

/*
 * Sort devices by devid, ascending
 */
static int cmp_device_id(void *priv, struct list_head *a,
		struct list_head *b)
{
	const struct btrfs_device *da = list_entry(a, struct btrfs_device,
			dev_list);
	const struct btrfs_device *db = list_entry(b, struct btrfs_device,
			dev_list);

	return da->devid < db->devid ? -1 :
		da->devid > db->devid ? 1 : 0;
}

static void print_one_uuid(struct btrfs_fs_devices *fs_devices)
{
	char uuidbuf[37];
	struct list_head *cur;
	struct btrfs_device *device;
	u64 devs_found = 0;
	u64 total;

	uuid_unparse(fs_devices->fsid, uuidbuf);
	device = list_entry(fs_devices->devices.next, struct btrfs_device,
			    dev_list);
	if (device->label && device->label[0])
		printf("Label: '%s' ", device->label);
	else
		printf("Label: none ");


	total = device->total_devs;
	printf(" uuid: %s\n\tTotal devices %llu FS bytes used %s\n", uuidbuf,
	       (unsigned long long)total,
	       pretty_size(device->super_bytes_used));

	list_sort(NULL, &fs_devices->devices, cmp_device_id);
	list_for_each(cur, &fs_devices->devices) {
		device = list_entry(cur, struct btrfs_device, dev_list);

		printf("\tdevid %4llu size %s used %s path %s\n",
		       (unsigned long long)device->devid,
		       pretty_size(device->total_bytes),
		       pretty_size(device->bytes_used), device->name);

		devs_found++;
	}
	if (devs_found < total) {
		printf("\t*** Some devices missing\n");
	}
	printf("\n");
}

static const char * const cmd_show_usage[] = {
	"btrfs filesystem show [--all-devices|<uuid>]",
	"Show the structure of a filesystem",
	"If no argument is given, structure of all present filesystems is shown.",
	NULL
};

static int cmd_show(int argc, char **argv)
{
	struct list_head *all_uuids;
	struct btrfs_fs_devices *fs_devices;
	struct list_head *cur_uuid;
	char *search = NULL;
	int ret;
	int where = BTRFS_SCAN_PROC;
	int searchstart = 1;

	if( argc > 1 && !strcmp(argv[1],"--all-devices")){
		where = BTRFS_SCAN_DEV;
		searchstart += 1;
	}

	if (check_argc_max(argc, searchstart + 1))
		usage(cmd_show_usage);

	ret = scan_for_btrfs(where, 0);

	if (ret){
		fprintf(stderr, "ERROR: error %d while scanning\n", ret);
		return 1;
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
	DIR	*dirstream = NULL;

	if (check_argc_exact(argc, 2))
		usage(cmd_sync_usage);

	path = argv[1];

	fd = open_file_or_dir(path, &dirstream);
	if (fd < 0) {
		fprintf(stderr, "ERROR: can't access to '%s'\n", path);
		return 1;
	}

	printf("FSSync '%s'\n", path);
	res = ioctl(fd, BTRFS_IOC_SYNC);
	e = errno;
	close_file_or_dir(fd, dirstream);
	if( res < 0 ){
		fprintf(stderr, "ERROR: unable to fs-syncing '%s' - %s\n", 
			path, strerror(e));
		return 1;
	}

	return 0;
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
	"-r             defragment files recursively",
	"-c[zlib,lzo]   compress the file while defragmenting",
	"-f             flush data to disk immediately after defragmenting",
	"-s start       defragment only from byte onward",
	"-l len         defragment only up to len bytes",
	"-t size        minimal size of file to be considered for defragmenting",
	NULL
};

static int do_defrag(int fd, int fancy_ioctl,
		struct btrfs_ioctl_defrag_range_args *range)
{
	int ret;

	if (!fancy_ioctl)
		ret = ioctl(fd, BTRFS_IOC_DEFRAG, NULL);
	else
		ret = ioctl(fd, BTRFS_IOC_DEFRAG_RANGE, range);

	return ret;
}

static int defrag_global_fancy_ioctl;
static struct btrfs_ioctl_defrag_range_args defrag_global_range;
static int defrag_global_verbose;
static int defrag_global_errors;
static int defrag_callback(const char *fpath, const struct stat *sb,
		int typeflag, struct FTW *ftwbuf)
{
	int ret = 0;
	int e = 0;
	int fd = 0;

	if (typeflag == FTW_F) {
		if (defrag_global_verbose)
			printf("%s\n", fpath);
		fd = open(fpath, O_RDWR);
		e = errno;
		if (fd < 0)
			goto error;
		ret = do_defrag(fd, defrag_global_fancy_ioctl, &defrag_global_range);
		e = errno;
		close(fd);
		if (ret && e == ENOTTY) {
			fprintf(stderr, "ERROR: defrag range ioctl not "
				"supported in this kernel, please try "
				"without any options.\n");
			defrag_global_errors++;
			return ENOTTY;
		}
		if (ret)
			goto error;
	}
	return 0;

error:
	fprintf(stderr, "ERROR: defrag failed on %s - %s\n", fpath, strerror(e));
	defrag_global_errors++;
	return 0;
}

static int cmd_defrag(int argc, char **argv)
{
	int fd;
	int flush = 0;
	u64 start = 0;
	u64 len = (u64)-1;
	u32 thresh = 0;
	int i;
	int recursive = 0;
	int ret = 0;
	struct btrfs_ioctl_defrag_range_args range;
	int e = 0;
	int compress_type = BTRFS_COMPRESS_NONE;
	DIR *dirstream;

	defrag_global_errors = 0;
	defrag_global_verbose = 0;
	defrag_global_errors = 0;
	defrag_global_fancy_ioctl = 0;
	optind = 1;
	while(1) {
		int c = getopt(argc, argv, "vrc::fs:l:t:");
		if (c < 0)
			break;

		switch(c) {
		case 'c':
			compress_type = BTRFS_COMPRESS_ZLIB;
			if (optarg)
				compress_type = parse_compress_type(optarg);
			defrag_global_fancy_ioctl = 1;
			break;
		case 'f':
			flush = 1;
			defrag_global_fancy_ioctl = 1;
			break;
		case 'v':
			defrag_global_verbose = 1;
			break;
		case 's':
			start = parse_size(optarg);
			defrag_global_fancy_ioctl = 1;
			break;
		case 'l':
			len = parse_size(optarg);
			defrag_global_fancy_ioctl = 1;
			break;
		case 't':
			thresh = parse_size(optarg);
			defrag_global_fancy_ioctl = 1;
			break;
		case 'r':
			recursive = 1;
			break;
		default:
			usage(cmd_defrag_usage);
		}
	}

	if (check_argc_min(argc - optind, 1))
		usage(cmd_defrag_usage);

	memset(&defrag_global_range, 0, sizeof(range));
	defrag_global_range.start = start;
	defrag_global_range.len = len;
	defrag_global_range.extent_thresh = thresh;
	if (compress_type) {
		defrag_global_range.flags |= BTRFS_DEFRAG_RANGE_COMPRESS;
		defrag_global_range.compress_type = compress_type;
	}
	if (flush)
		defrag_global_range.flags |= BTRFS_DEFRAG_RANGE_START_IO;

	for (i = optind; i < argc; i++) {
		dirstream = NULL;
		fd = open_file_or_dir(argv[i], &dirstream);
		if (fd < 0) {
			fprintf(stderr, "ERROR: failed to open %s - %s\n", argv[i],
					strerror(errno));
			defrag_global_errors++;
			close_file_or_dir(fd, dirstream);
			continue;
		}
		if (recursive) {
			struct stat st;

			fstat(fd, &st);
			if (S_ISDIR(st.st_mode)) {
				ret = nftw(argv[i], defrag_callback, 10,
						FTW_MOUNT | FTW_PHYS);
				if (ret == ENOTTY)
					exit(1);
				/* errors are handled in the callback */
				ret = 0;
			} else {
				if (defrag_global_verbose)
					printf("%s\n", argv[i]);
				ret = do_defrag(fd, defrag_global_fancy_ioctl,
						&defrag_global_range);
				e = errno;
			}
		} else {
			if (defrag_global_verbose)
				printf("%s\n", argv[i]);
			ret = do_defrag(fd, defrag_global_fancy_ioctl,
					&defrag_global_range);
			e = errno;
		}
		close_file_or_dir(fd, dirstream);
		if (ret && e == ENOTTY) {
			fprintf(stderr, "ERROR: defrag range ioctl not "
				"supported in this kernel, please try "
				"without any options.\n");
			defrag_global_errors++;
			break;
		}
		if (ret) {
			fprintf(stderr, "ERROR: defrag failed on %s - %s\n",
				argv[i], strerror(e));
			defrag_global_errors++;
		}
	}
	if (defrag_global_verbose)
		printf("%s\n", BTRFS_BUILD_VERSION);
	if (defrag_global_errors)
		fprintf(stderr, "total %d failures\n", defrag_global_errors);

	return !!defrag_global_errors;
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
	DIR	*dirstream = NULL;

	if (check_argc_exact(argc, 3))
		usage(cmd_resize_usage);

	amount = argv[1];
	path = argv[2];

	len = strlen(amount);
	if (len == 0 || len >= BTRFS_VOL_NAME_MAX) {
		fprintf(stderr, "ERROR: size value too long ('%s)\n",
			amount);
		return 1;
	}

	fd = open_file_or_dir(path, &dirstream);
	if (fd < 0) {
		fprintf(stderr, "ERROR: can't access to '%s'\n", path);
		return 1;
	}

	printf("Resize '%s' of '%s'\n", path, amount);
	strncpy_null(args.name, amount);
	res = ioctl(fd, BTRFS_IOC_RESIZE, &args);
	e = errno;
	close_file_or_dir(fd, dirstream);
	if( res < 0 ){
		fprintf(stderr, "ERROR: unable to resize '%s' - %s\n", 
			path, strerror(e));
		return 1;
	}
	return 0;
}

static const char * const cmd_label_usage[] = {
	"btrfs filesystem label [<device>|<mount_point>] [<newlabel>]",
	"Get or change the label of a filesystem",
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
		NULL_CMD_STRUCT
	}
};

int cmd_filesystem(int argc, char **argv)
{
	return handle_command_group(&filesystem_cmd_group, argc, argv);
}
