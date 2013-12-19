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
#include <mntent.h>
#include <linux/limits.h>
#include <getopt.h>

#include "kerncompat.h"
#include "ctree.h"
#include "ioctl.h"
#include "utils.h"
#include "volumes.h"
#include "version.h"
#include "commands.h"
#include "list_sort.h"
#include "disk-io.h"


/*
 * for btrfs fi show, we maintain a hash of fsids we've already printed.
 * This way we don't print dups if a given FS is mounted more than once.
 */
#define SEEN_FSID_HASH_SIZE 256

struct seen_fsid {
	u8 fsid[BTRFS_FSID_SIZE];
	struct seen_fsid *next;
};

static struct seen_fsid *seen_fsid_hash[SEEN_FSID_HASH_SIZE] = {NULL,};

static int add_seen_fsid(u8 *fsid)
{
	u8 hash = fsid[0];
	int slot = hash % SEEN_FSID_HASH_SIZE;
	struct seen_fsid *seen = seen_fsid_hash[slot];
	struct seen_fsid *alloc;

	if (!seen)
		goto insert;

	while (1) {
		if (memcmp(seen->fsid, fsid, BTRFS_FSID_SIZE) == 0)
			return -EEXIST;

		if (!seen->next)
			break;

		seen = seen->next;
	}

insert:

	alloc = malloc(sizeof(*alloc));
	if (!alloc)
		return -ENOMEM;

	alloc->next = NULL;
	memcpy(alloc->fsid, fsid, BTRFS_FSID_SIZE);

	if (seen)
		seen->next = alloc;
	else
		seen_fsid_hash[slot] = alloc;

	return 0;
}

static void free_seen_fsid(void)
{
	int slot;
	struct seen_fsid *seen;
	struct seen_fsid *next;

	for (slot = 0; slot < SEEN_FSID_HASH_SIZE; slot++) {
		seen = seen_fsid_hash[slot];
		while (seen) {
			next = seen->next;
			free(seen);
			seen = next;
		}
		seen_fsid_hash[slot] = NULL;
	}
}

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
		return -e;
	}
	/* This really should never happen */
	if (!sargs->total_spaces) {
		free(sargs);
		return -ENOENT;
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
		return -e;
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
		fprintf(stderr, "ERROR: can't access '%s'\n", path);
		return 1;
	}
	ret = get_df(fd, &sargs);

	if (!ret && sargs) {
		print_df(sargs);
		free(sargs);
	} else {
		fprintf(stderr, "ERROR: get_df failed %s\n", strerror(-ret));
	}

	close_file_or_dir(fd, dirstream);
	return !!ret;
}

static int match_search_item_kernel(__u8 *fsid, char *mnt, char *label,
					char *search)
{
	char uuidbuf[BTRFS_UUID_UNPARSED_SIZE];
	int search_len = strlen(search);

	search_len = min(search_len, BTRFS_UUID_UNPARSED_SIZE);
	uuid_unparse(fsid, uuidbuf);
	if (!strncmp(uuidbuf, search, search_len))
		return 1;

	if (strlen(label) && strcmp(label, search) == 0)
		return 1;

	if (strcmp(mnt, search) == 0)
		return 1;

	return 0;
}

static int uuid_search(struct btrfs_fs_devices *fs_devices, char *search)
{
	char uuidbuf[BTRFS_UUID_UNPARSED_SIZE];
	struct list_head *cur;
	struct btrfs_device *device;
	int search_len = strlen(search);

	search_len = min(search_len, BTRFS_UUID_UNPARSED_SIZE);
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
	char uuidbuf[BTRFS_UUID_UNPARSED_SIZE];
	struct list_head *cur;
	struct btrfs_device *device;
	u64 devs_found = 0;
	u64 total;

	if (add_seen_fsid(fs_devices->fsid))
		return;

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

/* adds up all the used spaces as reported by the space info ioctl
 */
static u64 calc_used_bytes(struct btrfs_ioctl_space_args *si)
{
	u64 ret = 0;
	int i;
	for (i = 0; i < si->total_spaces; i++)
		ret += si->spaces[i].used_bytes;
	return ret;
}

static int print_one_fs(struct btrfs_ioctl_fs_info_args *fs_info,
		struct btrfs_ioctl_dev_info_args *dev_info,
		struct btrfs_ioctl_space_args *space_info,
		char *label, char *path)
{
	int i;
	int fd;
	int missing = 0;
	char uuidbuf[BTRFS_UUID_UNPARSED_SIZE];
	struct btrfs_ioctl_dev_info_args *tmp_dev_info;
	int ret;

	ret = add_seen_fsid(fs_info->fsid);
	if (ret == -EEXIST)
		return 0;
	else if (ret)
		return ret;

	uuid_unparse(fs_info->fsid, uuidbuf);
	if (label && strlen(label))
		printf("Label: '%s' ", label);
	else
		printf("Label: none ");

	printf(" uuid: %s\n\tTotal devices %llu FS bytes used %s\n", uuidbuf,
			fs_info->num_devices,
			pretty_size(calc_used_bytes(space_info)));

	for (i = 0; i < fs_info->num_devices; i++) {
		tmp_dev_info = (struct btrfs_ioctl_dev_info_args *)&dev_info[i];

		/* Add check for missing devices even mounted */
		fd = open((char *)tmp_dev_info->path, O_RDONLY);
		if (fd < 0) {
			missing = 1;
			continue;
		}
		close(fd);
		printf("\tdevid %4llu size %s used %s path %s\n",
			tmp_dev_info->devid,
			pretty_size(tmp_dev_info->total_bytes),
			pretty_size(tmp_dev_info->bytes_used),
			tmp_dev_info->path);
	}

	if (missing)
		printf("\t*** Some devices missing\n");
	printf("\n");
	return 0;
}

static void handle_print(char *mnt, char *label)
{
	int fd;
	struct btrfs_ioctl_fs_info_args fs_info_arg;
	struct btrfs_ioctl_dev_info_args *dev_info_arg = NULL;
	struct btrfs_ioctl_space_args *space_info_arg;

	if (get_fs_info(mnt, &fs_info_arg, &dev_info_arg)) {
		fprintf(stdout, "ERROR: get_fs_info failed\n");
		return;
	}

	fd = open(mnt, O_RDONLY);
	if (fd != -1 && !get_df(fd, &space_info_arg)) {
		print_one_fs(&fs_info_arg, dev_info_arg,
				space_info_arg, label, mnt);
		kfree(space_info_arg);
	}
	if (fd != -1)
		close(fd);
	kfree(dev_info_arg);
}

/* This function checks if the given input parameter is
 * an uuid or a path
 * return -1: some error in the given input
 * return 0: unknow input
 * return 1: given input is uuid
 * return 2: given input is path
 */
static int check_arg_type(char *input)
{
	uuid_t	out;
	char path[PATH_MAX];

	if (!input)
		return -EINVAL;

	if (realpath(input, path)) {
		if (is_block_device(path) == 1)
			return BTRFS_ARG_BLKDEV;

		if (is_mount_point(path) == 1)
			return BTRFS_ARG_MNTPOINT;

		return BTRFS_ARG_UNKNOWN;
	}

	if (strlen(input) == (BTRFS_UUID_UNPARSED_SIZE - 1) &&
		!uuid_parse(input, out))
		return BTRFS_ARG_UUID;

	return BTRFS_ARG_UNKNOWN;
}

static int btrfs_scan_kernel_v2(void *search)
{
	int ret = 0;
	char label[BTRFS_LABEL_SIZE];
	char mnt[BTRFS_PATH_NAME_MAX + 1];
	struct btrfs_ioctl_fslist *fslist;
	struct btrfs_ioctl_fslist *fslist_saved;
	u64 cnt_fs;
	int cnt_mnt;
	__u8 *fsid;
	__u64 flags;
	int found = 0;

	ret = get_fslist(&fslist, &cnt_fs);
	if (ret)
		return ret;
	fslist_saved = fslist;
	while (cnt_fs--) {
		fsid = fslist->fsid;
		flags = fslist->flags;
		fslist++;
		if (!(flags & BTRFS_FS_MOUNTED))
			continue;
		memset(mnt, 0, BTRFS_PATH_NAME_MAX + 1);
		memset(label, 0, sizeof(label));
		ret = fsid_to_mntpt(fsid, mnt, &cnt_mnt);
		if (ret)
			break;

		if (get_label_mounted(mnt, label)) {
			ret = 1;
			break;
		}

		if (search && !match_search_item_kernel(fsid,
					mnt, label, search))
			continue;

		handle_print(mnt, label);
		if (search) {
			found = 1;
			break;
		}
	}
	kfree(fslist_saved);
	if (search && !found)
		return 1;
	return ret;
}

static int btrfs_scan_kernel(void *search)
{
	int ret = 0, fd;
	FILE *f;
	struct mntent *mnt;
	struct btrfs_ioctl_fs_info_args fs_info_arg;
	struct btrfs_ioctl_dev_info_args *dev_info_arg = NULL;
	struct btrfs_ioctl_space_args *space_info_arg;
	char label[BTRFS_LABEL_SIZE];

	f = setmntent("/proc/self/mounts", "r");
	if (f == NULL)
		return 1;

	memset(label, 0, sizeof(label));
	while ((mnt = getmntent(f)) != NULL) {
		if (strcmp(mnt->mnt_type, "btrfs"))
			continue;
		ret = get_fs_info(mnt->mnt_dir, &fs_info_arg,
				&dev_info_arg);
		if (ret)
			goto out;

		if (get_label_mounted(mnt->mnt_dir, label)) {
			kfree(dev_info_arg);
			ret = 1;
			goto out;
		}
		if (search && !match_search_item_kernel(fs_info_arg.fsid,
					mnt->mnt_dir, label, search)) {
			kfree(dev_info_arg);
			continue;
		}

		fd = open(mnt->mnt_dir, O_RDONLY);
		if ((fd != -1) && !get_df(fd, &space_info_arg)) {
			print_one_fs(&fs_info_arg, dev_info_arg,
					space_info_arg, label, mnt->mnt_dir);
			kfree(space_info_arg);
			memset(label, 0, sizeof(label));
		}
		if (fd != -1)
			close(fd);
		kfree(dev_info_arg);
		if (search)
			ret = 0;
	}
	if (search)
		ret = 1;

out:
	endmntent(f);
	return ret;
}

static int dev_to_fsid(char *dev, __u8 *fsid)
{
	struct btrfs_super_block *disk_super;
	char *buf;
	int ret;
	int fd;

	buf = malloc(4096);
	if (!buf)
		return -ENOMEM;

	fd = open(dev, O_RDONLY);
	if (fd < 0) {
		ret = -errno;
		free(buf);
		return ret;
	}

	disk_super = (struct btrfs_super_block *)buf;
	ret = btrfs_read_dev_super(fd, disk_super,
			BTRFS_SUPER_INFO_OFFSET);
	if (ret)
		goto out;

	memcpy(fsid, disk_super->fsid, BTRFS_FSID_SIZE);
	ret = 0;

out:
	close(fd);
	free(buf);
	return ret;
}

static const char * const cmd_show_usage[] = {
	"btrfs filesystem show [options] [<path>|<uuid>|<device>|label]",
	"Show the structure of a filesystem",
	"-d|--all-devices   show only disks under /dev containing btrfs filesystem",
	"-m|--mounted       show only mounted btrfs",
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
	int where = BTRFS_SCAN_LBLKID;
	int type = 0;
	char mp[BTRFS_PATH_NAME_MAX + 1];
	char path[PATH_MAX];
	__u8 fsid[BTRFS_FSID_SIZE];
	char uuid_buf[37];
	int found = 0;

	while (1) {
		int long_index;
		static struct option long_options[] = {
			{ "all-devices", no_argument, NULL, 'd'},
			{ "mounted", no_argument, NULL, 'm'},
			{ NULL, no_argument, NULL, 0 },
		};
		int c = getopt_long(argc, argv, "dm", long_options,
					&long_index);
		if (c < 0)
			break;
		switch (c) {
		case 'd':
			where = BTRFS_SCAN_DEV;
			break;
		case 'm':
			where = BTRFS_SCAN_MOUNTED;
			break;
		default:
			usage(cmd_show_usage);
		}
	}

	if (check_argc_max(argc, optind + 1))
		usage(cmd_show_usage);

	if (argc > optind) {
		search = argv[optind];
		if (strlen(search) == 0)
			usage(cmd_show_usage);
		type = check_arg_type(search);
		/*
		 * needs spl handling if input arg is block dev
		 * And if input arg is mount-point just print it
		 * right away
		 */
		if (type == BTRFS_ARG_BLKDEV) {
			if (where == BTRFS_SCAN_DEV) {
				/* we need to do this because
				 * legacy BTRFS_SCAN_DEV
				 * provides /dev/dm-x paths
				 */
				if (realpath(search, path))
					search = path;
			} else {
				ret = get_btrfs_mount(search,
						mp, sizeof(mp));
				if (!ret) {
					/* given block dev is mounted*/
					search = mp;
					type = BTRFS_ARG_MNTPOINT;
				} else {
					ret = dev_to_fsid(search, fsid);
					if (ret) {
						fprintf(stderr,
							"ERROR: No btrfs on %s\n",
							search);
						return 1;
					}
					uuid_unparse(fsid, uuid_buf);
					search = uuid_buf;
					type = BTRFS_ARG_UUID;
					goto devs_only;
				}
			}
		} else if (type == BTRFS_ARG_MNTPOINT) {
			char label[BTRFS_LABEL_SIZE];
			if (get_label_mounted(search, label))
				return 1;
			handle_print(search, label);
			return 0;
		}
	}

	if (where == BTRFS_SCAN_DEV)
		goto devs_only;

	/* show mounted btrfs */
	ret = btrfs_scan_kernel_v2(search);
	if (ret == -ENOTTY)
		ret = btrfs_scan_kernel(search);
	if (search && !ret) {
		/* since search is found we are done */
		goto out;
	}

	/* shows mounted only */
	if (where == BTRFS_SCAN_MOUNTED)
		goto out;

devs_only:
	ret = scan_for_btrfs(where, !BTRFS_UPDATE_KERNEL);

	if (ret) {
		fprintf(stderr, "ERROR: %d while scanning\n", ret);
		return 1;
	}
	
	all_uuids = btrfs_scanned_uuids();
	list_for_each(cur_uuid, all_uuids) {
		fs_devices = list_entry(cur_uuid, struct btrfs_fs_devices,
					list);
		if (search && uuid_search(fs_devices, search) == 0)
			continue;

		print_one_uuid(fs_devices);
		found = 1;
	}
	if (search && !found)
		ret = 1;

out:
	printf("%s\n", BTRFS_BUILD_VERSION);
	free_seen_fsid();
	return ret;
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
		fprintf(stderr, "ERROR: can't access '%s'\n", path);
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

	if ((typeflag == FTW_F) && S_ISREG(sb->st_mode)) {
		if (defrag_global_verbose)
			printf("%s\n", fpath);
		fd = open(fpath, O_RDWR);
		e = errno;
		if (fd < 0)
			goto error;
		ret = do_defrag(fd, defrag_global_fancy_ioctl, &defrag_global_range);
		e = errno;
		close(fd);
		if (ret && e == ENOTTY && defrag_global_fancy_ioctl) {
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
		struct stat st;

		dirstream = NULL;
		fd = open_file_or_dir(argv[i], &dirstream);
		if (fd < 0) {
			fprintf(stderr, "ERROR: failed to open %s - %s\n", argv[i],
					strerror(errno));
			defrag_global_errors++;
			close_file_or_dir(fd, dirstream);
			continue;
		}
		if (fstat(fd, &st)) {
			fprintf(stderr, "ERROR: failed to stat %s - %s\n",
					argv[i], strerror(errno));
			defrag_global_errors++;
			close_file_or_dir(fd, dirstream);
			continue;
		}
		if (!(S_ISDIR(st.st_mode) || S_ISREG(st.st_mode))) {
			fprintf(stderr,
			    "ERROR: %s is not a directory or a regular file\n",
			    argv[i]);
			defrag_global_errors++;
			close_file_or_dir(fd, dirstream);
			continue;
		}
		if (recursive) {
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
		if (ret && e == ENOTTY && defrag_global_fancy_ioctl) {
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
		fprintf(stderr, "ERROR: can't access '%s'\n", path);
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

	if (argc > 2) {
		return set_label(argv[1], argv[2]);
	} else {
		char label[BTRFS_LABEL_SIZE];
		int ret;

		ret = get_label(argv[1], label);
		if (!ret)
			fprintf(stdout, "%s\n", label);

		return ret;
	}
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
