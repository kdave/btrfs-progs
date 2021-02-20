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
#include <fcntl.h>
#include <ftw.h>
#include <mntent.h>
#include <linux/limits.h>
#include <linux/version.h>
#include <getopt.h>
#include <limits.h>

#include <btrfsutil.h>

#include "kerncompat.h"
#include "kernel-shared/ctree.h"
#include "common/utils.h"
#include "kernel-shared/volumes.h"
#include "cmds/commands.h"
#include "cmds/filesystem-usage.h"
#include "kernel-lib/list_sort.h"
#include "kernel-shared/disk-io.h"
#include "common/help.h"
#include "common/fsfeatures.h"
#include "common/path-utils.h"
#include "common/device-scan.h"

/*
 * for btrfs fi show, we maintain a hash of fsids we've already printed.
 * This way we don't print dups if a given FS is mounted more than once.
 */
static struct seen_fsid *seen_fsid_hash[SEEN_FSID_HASH_SIZE] = {NULL,};
static mode_t defrag_open_mode = O_RDONLY;

static const char * const filesystem_cmd_group_usage[] = {
	"btrfs filesystem [<group>] <command> [<args>]",
	NULL
};

static const char * const cmd_filesystem_df_usage[] = {
	"btrfs filesystem df [options] <path>",
	"Show space usage information for a mount point",
	"",
	HELPINFO_UNITS_SHORT_LONG,
	NULL
};

static void print_df(struct btrfs_ioctl_space_args *sargs, unsigned unit_mode)
{
	u64 i;
	struct btrfs_ioctl_space_info *sp = sargs->spaces;

	for (i = 0; i < sargs->total_spaces; i++, sp++) {
		printf("%s, %s: total=%s, used=%s\n",
			btrfs_group_type_str(sp->flags),
			btrfs_group_profile_str(sp->flags),
			pretty_size_mode(sp->total_bytes, unit_mode),
			pretty_size_mode(sp->used_bytes, unit_mode));
	}
}

static int cmd_filesystem_df(const struct cmd_struct *cmd,
			     int argc, char **argv)
{
	struct btrfs_ioctl_space_args *sargs = NULL;
	int ret;
	int fd;
	char *path;
	DIR *dirstream = NULL;
	unsigned unit_mode;

	unit_mode = get_unit_mode_from_arg(&argc, argv, 1);

	clean_args_no_options(cmd, argc, argv);

	if (check_argc_exact(argc - optind, 1))
		return 1;

	path = argv[optind];

	fd = btrfs_open_dir(path, &dirstream, 1);
	if (fd < 0)
		return 1;

	ret = get_df(fd, &sargs);

	if (ret == 0) {
		print_df(sargs, unit_mode);
		free(sargs);
	} else {
		errno = -ret;
		error("get_df failed: %m");
	}

	btrfs_warn_multiple_profiles(fd);
	close_file_or_dir(fd, dirstream);
	return !!ret;
}
static DEFINE_SIMPLE_COMMAND(filesystem_df, "df");

static int match_search_item_kernel(u8 *fsid, char *mnt, char *label,
					char *search)
{
	char uuidbuf[BTRFS_UUID_UNPARSED_SIZE];
	int search_len = strlen(search);

	search_len = min(search_len, BTRFS_UUID_UNPARSED_SIZE);
	uuid_unparse(fsid, uuidbuf);
	if (!strncmp(uuidbuf, search, search_len))
		return 1;

	if (*label && strcmp(label, search) == 0)
		return 1;

	if (strcmp(mnt, search) == 0)
		return 1;

	return 0;
}

/* Search for user visible uuid 'search' in registered filesystems */
static int uuid_search(struct btrfs_fs_devices *fs_devices, const char *search)
{
	char uuidbuf[BTRFS_UUID_UNPARSED_SIZE];
	struct btrfs_device *device;
	int search_len = strlen(search);

	search_len = min(search_len, BTRFS_UUID_UNPARSED_SIZE);
	uuid_unparse(fs_devices->fsid, uuidbuf);
	if (!strncmp(uuidbuf, search, search_len))
		return 1;

	list_for_each_entry(device, &fs_devices->devices, dev_list) {
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

static void splice_device_list(struct list_head *seed_devices,
			       struct list_head *all_devices)
{
	struct btrfs_device *in_all, *next_all;
	struct btrfs_device *in_seed, *next_seed;

	list_for_each_entry_safe(in_all, next_all, all_devices, dev_list) {
		list_for_each_entry_safe(in_seed, next_seed, seed_devices,
								dev_list) {
			if (in_all->devid == in_seed->devid) {
				/*
				 * When do dev replace in a sprout fs
				 * to a dev in its seed fs, the replacing
				 * dev will reside in the sprout fs and
				 * the replaced dev will still exist
				 * in the seed fs.
				 * So pick the latest one when showing
				 * the sprout fs.
				 */
				if (in_all->generation
						< in_seed->generation) {
					list_del(&in_all->dev_list);
					free(in_all);
				} else if (in_all->generation
						> in_seed->generation) {
					list_del(&in_seed->dev_list);
					free(in_seed);
				}
				break;
			}
		}
	}

	list_splice(seed_devices, all_devices);
}

static void print_devices(struct btrfs_fs_devices *fs_devices,
			  u64 *devs_found, unsigned unit_mode)
{
	struct btrfs_device *device;
	struct btrfs_fs_devices *cur_fs;
	struct list_head *all_devices;

	all_devices = &fs_devices->devices;
	cur_fs = fs_devices->seed;
	/* add all devices of seed fs to the fs to be printed */
	while (cur_fs) {
		splice_device_list(&cur_fs->devices, all_devices);
		cur_fs = cur_fs->seed;
	}

	list_sort(NULL, all_devices, cmp_device_id);
	list_for_each_entry(device, all_devices, dev_list) {
		printf("\tdevid %4llu size %s used %s path %s\n",
		       (unsigned long long)device->devid,
		       pretty_size_mode(device->total_bytes, unit_mode),
		       pretty_size_mode(device->bytes_used, unit_mode),
		       device->name);

		(*devs_found)++;
	}
}

static void print_one_uuid(struct btrfs_fs_devices *fs_devices,
			   unsigned unit_mode)
{
	char uuidbuf[BTRFS_UUID_UNPARSED_SIZE];
	struct btrfs_device *device;
	u64 devs_found = 0;
	u64 total;

	if (add_seen_fsid(fs_devices->fsid, seen_fsid_hash, -1, NULL))
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
	       pretty_size_mode(device->super_bytes_used, unit_mode));

	print_devices(fs_devices, &devs_found, unit_mode);

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
		char *label, unsigned unit_mode)
{
	int i;
	int fd;
	int missing = 0;
	char uuidbuf[BTRFS_UUID_UNPARSED_SIZE];
	struct btrfs_ioctl_dev_info_args *tmp_dev_info;
	int ret;

	ret = add_seen_fsid(fs_info->fsid, seen_fsid_hash, -1, NULL);
	if (ret == -EEXIST)
		return 0;
	else if (ret)
		return ret;

	uuid_unparse(fs_info->fsid, uuidbuf);
	if (label && *label)
		printf("Label: '%s' ", label);
	else
		printf("Label: none ");

	printf(" uuid: %s\n\tTotal devices %llu FS bytes used %s\n", uuidbuf,
			fs_info->num_devices,
			pretty_size_mode(calc_used_bytes(space_info),
					 unit_mode));

	for (i = 0; i < fs_info->num_devices; i++) {
		char *canonical_path;

		tmp_dev_info = (struct btrfs_ioctl_dev_info_args *)&dev_info[i];

		/* Add check for missing devices even mounted */
		fd = open((char *)tmp_dev_info->path, O_RDONLY);
		if (fd < 0) {
			missing = 1;
			continue;
		}
		close(fd);
		canonical_path = path_canonicalize((char *)tmp_dev_info->path);
		printf("\tdevid %4llu size %s used %s path %s\n",
			tmp_dev_info->devid,
			pretty_size_mode(tmp_dev_info->total_bytes, unit_mode),
			pretty_size_mode(tmp_dev_info->bytes_used, unit_mode),
			canonical_path);

		free(canonical_path);
	}

	if (missing)
		printf("\t*** Some devices missing\n");
	printf("\n");
	return 0;
}

static int btrfs_scan_kernel(void *search, unsigned unit_mode)
{
	int ret = 0, fd;
	int found = 0;
	FILE *f;
	struct mntent *mnt;
	struct btrfs_ioctl_fs_info_args fs_info_arg;
	struct btrfs_ioctl_dev_info_args *dev_info_arg = NULL;
	struct btrfs_ioctl_space_args *space_info_arg = NULL;
	char label[BTRFS_LABEL_SIZE];

	f = setmntent("/proc/self/mounts", "r");
	if (f == NULL)
		return 1;

	memset(label, 0, sizeof(label));
	while ((mnt = getmntent(f)) != NULL) {
		free(dev_info_arg);
		dev_info_arg = NULL;
		if (strcmp(mnt->mnt_type, "btrfs"))
			continue;
		ret = get_fs_info(mnt->mnt_dir, &fs_info_arg,
				&dev_info_arg);
		if (ret)
			goto out;

		/* skip all fs already shown as mounted fs */
		if (is_seen_fsid(fs_info_arg.fsid, seen_fsid_hash))
			continue;

		ret = get_label_mounted(mnt->mnt_dir, label);
		/* provide backward kernel compatibility */
		if (ret == -ENOTTY)
			ret = get_label_unmounted(
				(const char *)dev_info_arg->path, label);

		if (ret)
			goto out;

		if (search && !match_search_item_kernel(fs_info_arg.fsid,
					mnt->mnt_dir, label, search)) {
			continue;
		}

		fd = open(mnt->mnt_dir, O_RDONLY);
		if ((fd != -1) && !get_df(fd, &space_info_arg)) {
			print_one_fs(&fs_info_arg, dev_info_arg,
				     space_info_arg, label, unit_mode);
			free(space_info_arg);
			memset(label, 0, sizeof(label));
			found = 1;
		}
		if (fd != -1)
			close(fd);
	}

out:
	free(dev_info_arg);
	endmntent(f);
	return !found;
}

static void free_fs_devices(struct btrfs_fs_devices *fs_devices)
{
	struct btrfs_fs_devices *cur_seed, *next_seed;
	struct btrfs_device *device;

	while (!list_empty(&fs_devices->devices)) {
		device = list_entry(fs_devices->devices.next,
					struct btrfs_device, dev_list);
		list_del(&device->dev_list);

		free(device->name);
		free(device->label);
		free(device);
	}

	/* free seed fs chain */
	cur_seed = fs_devices->seed;
	fs_devices->seed = NULL;
	while (cur_seed) {
		next_seed = cur_seed->seed;
		free(cur_seed);

		cur_seed = next_seed;
	}

	list_del(&fs_devices->list);
	free(fs_devices);
}

static int copy_device(struct btrfs_device *dst,
		       struct btrfs_device *src)
{
	dst->devid = src->devid;
	memcpy(dst->uuid, src->uuid, BTRFS_UUID_SIZE);
	if (src->name == NULL)
		dst->name = NULL;
	else {
		dst->name = strdup(src->name);
		if (!dst->name)
			return -ENOMEM;
	}
	if (src->label == NULL)
		dst->label = NULL;
	else {
		dst->label = strdup(src->label);
		if (!dst->label) {
			free(dst->name);
			return -ENOMEM;
		}
	}
	dst->total_devs = src->total_devs;
	dst->super_bytes_used = src->super_bytes_used;
	dst->total_bytes = src->total_bytes;
	dst->bytes_used = src->bytes_used;
	dst->generation = src->generation;

	return 0;
}

static int copy_fs_devices(struct btrfs_fs_devices *dst,
			   struct btrfs_fs_devices *src)
{
	struct btrfs_device *cur_dev, *dev_copy;
	int ret = 0;

	memcpy(dst->fsid, src->fsid, BTRFS_FSID_SIZE);
	memcpy(dst->metadata_uuid, src->metadata_uuid, BTRFS_FSID_SIZE);
	INIT_LIST_HEAD(&dst->devices);
	dst->seed = NULL;

	list_for_each_entry(cur_dev, &src->devices, dev_list) {
		dev_copy = malloc(sizeof(*dev_copy));
		if (!dev_copy) {
			ret = -ENOMEM;
			break;
		}

		ret = copy_device(dev_copy, cur_dev);
		if (ret) {
			free(dev_copy);
			break;
		}

		list_add(&dev_copy->dev_list, &dst->devices);
		dev_copy->fs_devices = dst;
	}

	return ret;
}

static int find_and_copy_seed(struct btrfs_fs_devices *seed,
			      struct btrfs_fs_devices *copy,
			      struct list_head *fs_uuids) {
	struct btrfs_fs_devices *cur_fs;

	list_for_each_entry(cur_fs, fs_uuids, list)
		if (!memcmp(seed->fsid, cur_fs->fsid, BTRFS_FSID_SIZE))
			return copy_fs_devices(copy, cur_fs);

	return 1;
}

static int has_seed_devices(struct btrfs_fs_devices *fs_devices)
{
	struct btrfs_device *device;
	int dev_cnt_total, dev_cnt = 0;

	device = list_first_entry(&fs_devices->devices, struct btrfs_device,
				  dev_list);

	dev_cnt_total = device->total_devs;

	list_for_each_entry(device, &fs_devices->devices, dev_list)
		dev_cnt++;

	return dev_cnt_total != dev_cnt;
}

static int search_umounted_fs_uuids(struct list_head *all_uuids,
				    char *search, int *found)
{
	struct btrfs_fs_devices *cur_fs, *fs_copy;
	struct list_head *fs_uuids;
	int ret = 0;

	fs_uuids = btrfs_scanned_uuids();

	/*
	 * The fs_uuids list is global, and open_ctree_* will
	 * modify it, make a private copy here
	 */
	list_for_each_entry(cur_fs, fs_uuids, list) {
		/* don't bother handle all fs, if search target specified */
		if (search) {
			if (uuid_search(cur_fs, search) == 0)
				continue;
			if (found)
				*found = 1;
		}

		/* skip all fs already shown as mounted fs */
		if (is_seen_fsid(cur_fs->fsid, seen_fsid_hash))
			continue;

		fs_copy = calloc(1, sizeof(*fs_copy));
		if (!fs_copy) {
			ret = -ENOMEM;
			goto out;
		}

		ret = copy_fs_devices(fs_copy, cur_fs);
		if (ret) {
			free(fs_copy);
			goto out;
		}

		list_add(&fs_copy->list, all_uuids);
	}

out:
	return ret;
}

static int map_seed_devices(struct list_head *all_uuids)
{
	struct btrfs_fs_devices *cur_fs, *cur_seed;
	struct btrfs_fs_devices *seed_copy;
	struct btrfs_fs_devices *opened_fs;
	struct btrfs_device *device;
	struct btrfs_fs_info *fs_info;
	struct list_head *fs_uuids;
	int ret = 0;

	fs_uuids = btrfs_scanned_uuids();

	list_for_each_entry(cur_fs, all_uuids, list) {
		device = list_first_entry(&cur_fs->devices,
						struct btrfs_device, dev_list);
		if (!device)
			continue;

		/* skip fs without seeds */
		if (!has_seed_devices(cur_fs))
			continue;

		/*
		 * open_ctree_* detects seed/sprout mapping
		 */
		fs_info = open_ctree_fs_info(device->name, 0, 0, 0,
						OPEN_CTREE_PARTIAL);
		if (!fs_info)
			continue;

		/*
		 * copy the seed chain under the opened fs
		 */
		opened_fs = fs_info->fs_devices;
		cur_seed = cur_fs;
		while (opened_fs->seed) {
			seed_copy = malloc(sizeof(*seed_copy));
			if (!seed_copy) {
				ret = -ENOMEM;
				goto fail_out;
			}
			ret = find_and_copy_seed(opened_fs->seed, seed_copy,
						 fs_uuids);
			if (ret) {
				free(seed_copy);
				goto fail_out;
			}

			cur_seed->seed = seed_copy;

			opened_fs = opened_fs->seed;
			cur_seed = cur_seed->seed;
		}

		close_ctree(fs_info->chunk_root);
	}

out:
	return ret;
fail_out:
	close_ctree(fs_info->chunk_root);
	goto out;
}

static const char * const cmd_filesystem_show_usage[] = {
	"btrfs filesystem show [options] [<path>|<uuid>|<device>|label]",
	"Show the structure of a filesystem",
	"",
	"-d|--all-devices   show only disks under /dev containing btrfs filesystem",
	"-m|--mounted       show only mounted btrfs",
	HELPINFO_UNITS_LONG,
	"",
	"If no argument is given, structure of all present filesystems is shown.",
	NULL
};

static int cmd_filesystem_show(const struct cmd_struct *cmd,
			       int argc, char **argv)
{
	LIST_HEAD(all_uuids);
	struct btrfs_fs_devices *fs_devices;
	char *search = NULL;
	int ret;
	/* default, search both kernel and udev */
	int where = -1;
	int type = 0;
	char mp[PATH_MAX];
	char path[PATH_MAX];
	u8 fsid[BTRFS_FSID_SIZE];
	char uuid_buf[BTRFS_UUID_UNPARSED_SIZE];
	unsigned unit_mode;
	int found = 0;

	unit_mode = get_unit_mode_from_arg(&argc, argv, 0);

	optind = 0;
	while (1) {
		int c;
		static const struct option long_options[] = {
			{ "all-devices", no_argument, NULL, 'd'},
			{ "mounted", no_argument, NULL, 'm'},
			{ NULL, 0, NULL, 0 }
		};

		c = getopt_long(argc, argv, "dm", long_options, NULL);
		if (c < 0)
			break;
		switch (c) {
		case 'd':
			where = BTRFS_SCAN_LBLKID;
			break;
		case 'm':
			where = BTRFS_SCAN_MOUNTED;
			break;
		default:
			usage_unknown_option(cmd, argv);
		}
	}

	if (check_argc_max(argc, optind + 1))
		return 1;

	if (argc > optind) {
		search = argv[optind];
		if (*search == 0)
			usage(cmd);
		type = check_arg_type(search);

		/*
		 * For search is a device:
		 *     realpath do /dev/mapper/XX => /dev/dm-X
		 *     which is required by BTRFS_SCAN_DEV
		 * For search is a mountpoint:
		 *     realpath do  /mnt/btrfs/  => /mnt/btrfs
		 *     which shall be recognized by btrfs_scan_kernel()
		 */
		if (realpath(search, path))
			search = path;

		/*
		 * Needs special handling if input arg is block dev And if
		 * input arg is mount-point just print it right away
		 */
		if (type == BTRFS_ARG_BLKDEV && where != BTRFS_SCAN_LBLKID) {
			ret = get_btrfs_mount(search, mp, sizeof(mp));
			if (!ret) {
				/* given block dev is mounted */
				search = mp;
				type = BTRFS_ARG_MNTPOINT;
			} else {
				ret = dev_to_fsid(search, fsid);
				if (ret) {
					error("no btrfs on %s", search);
					return 1;
				}
				uuid_unparse(fsid, uuid_buf);
				search = uuid_buf;
				type = BTRFS_ARG_UUID;
				goto devs_only;
			}
		}
	}

	if (where == BTRFS_SCAN_LBLKID)
		goto devs_only;

	/* show mounted btrfs */
	ret = btrfs_scan_kernel(search, unit_mode);
	if (search && !ret) {
		/* since search is found we are done */
		goto out;
	}

	/* shows mounted only */
	if (where == BTRFS_SCAN_MOUNTED)
		goto out;

devs_only:
	if (type == BTRFS_ARG_REG) {
		/*
		 * We don't close the fs_info because it will free the device,
		 * this is not a long-running process so it's fine
		 */
		if (open_ctree(search, btrfs_sb_offset(0), 0))
			ret = 0;
		else
			ret = 1;
	} else {
		ret = btrfs_scan_devices(0);
	}

	if (ret) {
		error("blkid device scan returned %d", ret);
		return 1;
	}

	ret = search_umounted_fs_uuids(&all_uuids, search, &found);
	if (ret < 0) {
		error("searching target device returned error %d", ret);
		return 1;
	}

	/*
	 * The seed/sprout mapping are not detected yet,
	 * do mapping build for all umounted fs
	 */
	ret = map_seed_devices(&all_uuids);
	if (ret) {
		error("mapping seed devices returned error %d", ret);
		return 1;
	}

	list_for_each_entry(fs_devices, &all_uuids, list)
		print_one_uuid(fs_devices, unit_mode);

	if (search && !found) {
		error("not a valid btrfs filesystem: %s", search);
		ret = 1;
	}
	while (!list_empty(&all_uuids)) {
		fs_devices = list_entry(all_uuids.next,
					struct btrfs_fs_devices, list);
		free_fs_devices(fs_devices);
	}
out:
	free_seen_fsid(seen_fsid_hash);
	return ret;
}
static DEFINE_SIMPLE_COMMAND(filesystem_show, "show");

static const char * const cmd_filesystem_sync_usage[] = {
	"btrfs filesystem sync <path>",
	"Force a sync on a filesystem",
	NULL
};

static int cmd_filesystem_sync(const struct cmd_struct *cmd,
			       int argc, char **argv)
{
	enum btrfs_util_error err;

	clean_args_no_options(cmd, argc, argv);

	if (check_argc_exact(argc - optind, 1))
		return 1;

	err = btrfs_util_sync(argv[optind]);
	if (err) {
		error_btrfs_util(err);
		return 1;
	}

	return 0;
}
static DEFINE_SIMPLE_COMMAND(filesystem_sync, "sync");

static int parse_compress_type(char *s)
{
	if (strcmp(optarg, "zlib") == 0)
		return BTRFS_COMPRESS_ZLIB;
	else if (strcmp(optarg, "lzo") == 0)
		return BTRFS_COMPRESS_LZO;
	else if (strcmp(optarg, "zstd") == 0)
		return BTRFS_COMPRESS_ZSTD;
	else {
		error("unknown compression type %s", s);
		exit(1);
	};
}

static const char * const cmd_filesystem_defrag_usage[] = {
	"btrfs filesystem defragment [options] <file>|<dir> [<file>|<dir>...]",
	"Defragment a file or a directory",
	"",
	"-r                  defragment files recursively",
	"-c[zlib,lzo,zstd]   compress the file while defragmenting",
	"-f                  flush data to disk immediately after defragmenting",
	"-s start            defragment only from byte onward",
	"-l len              defragment only up to len bytes",
	"-t size             target extent size hint (default: 32M)",
	"-v                  deprecated, alias for global -v option",
	HELPINFO_INSERT_GLOBALS,
	HELPINFO_INSERT_VERBOSE,
	"",
	"Warning: most Linux kernels will break up the ref-links of COW data",
	"(e.g., files copied with 'cp --reflink', snapshots) which may cause",
	"considerable increase of space usage. See btrfs-filesystem(8) for",
	"more information.",
	NULL
};

static struct btrfs_ioctl_defrag_range_args defrag_global_range;
static int defrag_global_errors;
static int defrag_callback(const char *fpath, const struct stat *sb,
		int typeflag, struct FTW *ftwbuf)
{
	int ret = 0;
	int fd = 0;

	if ((typeflag == FTW_F) && S_ISREG(sb->st_mode)) {
		pr_verbose(1, "%s\n", fpath);
		fd = open(fpath, defrag_open_mode);
		if (fd < 0) {
			goto error;
		}
		ret = ioctl(fd, BTRFS_IOC_DEFRAG_RANGE, &defrag_global_range);
		close(fd);
		if (ret && errno == ENOTTY) {
			error(
"defrag range ioctl not supported in this kernel version, 2.6.33 and newer is required");
			defrag_global_errors++;
			return ENOTTY;
		}
		if (ret) {
			goto error;
		}
	}
	return 0;

error:
	error("defrag failed on %s: %m", fpath);
	defrag_global_errors++;
	return 0;
}

static int cmd_filesystem_defrag(const struct cmd_struct *cmd,
				 int argc, char **argv)
{
	int fd;
	int flush = 0;
	u64 start = 0;
	u64 len = (u64)-1;
	u64 thresh;
	int i;
	int recursive = 0;
	int ret = 0;
	int compress_type = BTRFS_COMPRESS_NONE;
	DIR *dirstream;

	/*
	 * Kernel 4.19+ supports defragmention of files open read-only,
	 * otherwise it's an ETXTBSY error
	 */
	if (get_running_kernel_version() < KERNEL_VERSION(4,19,0))
		defrag_open_mode = O_RDWR;

	/*
	 * Kernel has a different default (256K) that is supposed to be safe,
	 * but it does not defragment very well. The 32M will likely lead to
	 * better results and is independent of the kernel default. We have to
	 * use the v2 defrag ioctl.
	 */
	thresh = SZ_32M;

	defrag_global_errors = 0;
	defrag_global_errors = 0;
	optind = 0;
	while(1) {
		int c = getopt(argc, argv, "vrc::fs:l:t:");
		if (c < 0)
			break;

		switch(c) {
		case 'c':
			compress_type = BTRFS_COMPRESS_ZLIB;
			if (optarg)
				compress_type = parse_compress_type(optarg);
			break;
		case 'f':
			flush = 1;
			break;
		case 'v':
			bconf_be_verbose();
			break;
		case 's':
			start = parse_size_from_string(optarg);
			break;
		case 'l':
			len = parse_size_from_string(optarg);
			break;
		case 't':
			thresh = parse_size_from_string(optarg);
			if (thresh > (u32)-1) {
				warning(
			    "target extent size %llu too big, trimmed to %u",
					thresh, (u32)-1);
				thresh = (u32)-1;
			}
			break;
		case 'r':
			recursive = 1;
			break;
		default:
			usage_unknown_option(cmd, argv);
		}
	}

	if (check_argc_min(argc - optind, 1))
		return 1;

	memset(&defrag_global_range, 0, sizeof(defrag_global_range));
	defrag_global_range.start = start;
	defrag_global_range.len = len;
	defrag_global_range.extent_thresh = (u32)thresh;
	if (compress_type) {
		defrag_global_range.flags |= BTRFS_DEFRAG_RANGE_COMPRESS;
		defrag_global_range.compress_type = compress_type;
	}
	if (flush)
		defrag_global_range.flags |= BTRFS_DEFRAG_RANGE_START_IO;

	/*
	 * Look for directory arguments and warn if the recursive mode is not
	 * requested, as this is not implemented as recursive defragmentation
	 * in kernel. The stat errors are silent here as we check them below.
	 */
	if (!recursive) {
		int found = 0;

		for (i = optind; i < argc; i++) {
			struct stat st;

			if (stat(argv[i], &st))
				continue;

			if (S_ISDIR(st.st_mode)) {
				warning(
			"directory specified but recursive mode not requested: %s",
					argv[i]);
				found = 1;
			}
		}
		if (found) {
			warning(
"a directory passed to the defrag ioctl will not process the files\n"
"recursively but will defragment the subvolume tree and the extent tree.\n"
"If this is not intended, please use option -r .");
		}
	}

	for (i = optind; i < argc; i++) {
		struct stat st;
		int defrag_err = 0;

		dirstream = NULL;
		fd = open_file_or_dir3(argv[i], &dirstream, defrag_open_mode);
		if (fd < 0) {
			error("cannot open %s: %m", argv[i]);
			ret = -errno;
			goto next;
		}

		ret = fstat(fd, &st);
		if (ret) {
			error("failed to stat %s: %m", argv[i]);
			ret = -errno;
			goto next;
		}
		if (!(S_ISDIR(st.st_mode) || S_ISREG(st.st_mode))) {
			error("%s is not a directory or a regular file",
					argv[i]);
			ret = -EINVAL;
			goto next;
		}
		if (recursive && S_ISDIR(st.st_mode)) {
			ret = nftw(argv[i], defrag_callback, 10,
						FTW_MOUNT | FTW_PHYS);
			if (ret == ENOTTY)
				exit(1);
			/* errors are handled in the callback */
			ret = 0;
		} else {
			pr_verbose(1, "%s\n", argv[i]);
			ret = ioctl(fd, BTRFS_IOC_DEFRAG_RANGE,
					&defrag_global_range);
			defrag_err = errno;
			if (ret && defrag_err == ENOTTY) {
				error(
"defrag range ioctl not supported in this kernel version, 2.6.33 and newer is required");
				defrag_global_errors++;
				close_file_or_dir(fd, dirstream);
				break;
			}
			if (ret) {
				errno = defrag_err;
				error("defrag failed on %s: %m", argv[i]);
				goto next;
			}
		}
next:
		if (ret)
			defrag_global_errors++;
		close_file_or_dir(fd, dirstream);
	}

	if (defrag_global_errors)
		fprintf(stderr, "total %d failures\n", defrag_global_errors);

	return !!defrag_global_errors;
}
static DEFINE_SIMPLE_COMMAND(filesystem_defrag, "defragment");

static const char * const cmd_filesystem_resize_usage[] = {
	"btrfs filesystem resize [options] [devid:][+/-]<newsize>[kKmMgGtTpPeE]|[devid:]max <path>",
	"Resize a filesystem",
	"If 'max' is passed, the filesystem will occupy all available space",
	"on the device 'devid'.",
	"[kK] means KiB, which denotes 1KiB = 1024B, 1MiB = 1024KiB, etc.",
	"",
	"--enqueue         wait if there's another exclusive operation running,",
	"                  otherwise continue",
	NULL
};

static int check_resize_args(const char *amount, const char *path) {
	struct btrfs_ioctl_fs_info_args fi_args;
	struct btrfs_ioctl_dev_info_args *di_args = NULL;
	int ret, i, dev_idx = -1;
	u64 devid = 1;
	const char *res_str = NULL;
	char *devstr = NULL, *sizestr = NULL;
	u64 new_size = 0, old_size = 0, diff = 0;
	int mod = 0;
	char amount_dup[BTRFS_VOL_NAME_MAX];

	ret = get_fs_info(path, &fi_args, &di_args);

	if (ret) {
		error("unable to retrieve fs info");
		return 1;
	}

	if (!fi_args.num_devices) {
		error("no devices found");
		ret = 1;
		goto out;
	}

	ret = snprintf(amount_dup, BTRFS_VOL_NAME_MAX, "%s", amount);
	if (strlen(amount) != ret) {
		error("newsize argument is too long");
		ret = 1;
		goto out;
	}

	sizestr = amount_dup;
	devstr = strchr(sizestr, ':');
	if (devstr) {
		sizestr = devstr + 1;
		*devstr = 0;
		devstr = amount_dup;

		errno = 0;
		devid = strtoull(devstr, NULL, 10);

		if (errno) {
			error("failed to parse devid %s: %m", devstr);
			ret = 1;
			goto out;
		}
	}

	dev_idx = -1;
	for(i = 0; i < fi_args.num_devices; i++) {
		if (di_args[i].devid == devid) {
			dev_idx = i;
			break;
		}
	}

	if (dev_idx < 0) {
		error("cannot find devid: %lld", devid);
		ret = 1;
		goto out;
	}

	if (strcmp(sizestr, "max") == 0) {
		res_str = "max";
	} else {
		if (sizestr[0] == '-') {
			mod = -1;
			sizestr++;
		} else if (sizestr[0] == '+') {
			mod = 1;
			sizestr++;
		}
		diff = parse_size_from_string(sizestr);
		if (!diff) {
			error("failed to parse size %s", sizestr);
			ret = 1;
			goto out;
		}
		old_size = di_args[dev_idx].total_bytes;

		if (mod < 0) {
			if (diff > old_size) {
				error("current size is %s which is smaller than %s",
				      pretty_size_mode(old_size, UNITS_DEFAULT),
				      pretty_size_mode(diff, UNITS_DEFAULT));
				ret = 1;
				goto out;
			}
			new_size = old_size - diff;
		} else if (mod > 0) {
			if (diff > ULLONG_MAX - old_size) {
				error("increasing %s is out of range",
				      pretty_size_mode(diff, UNITS_DEFAULT));
				ret = 1;
				goto out;
			}
			new_size = old_size + diff;
		}
		new_size = round_down(new_size, fi_args.sectorsize);
		res_str = pretty_size_mode(new_size, UNITS_DEFAULT);
	}

	printf("Resize device id %lld (%s) from %s to %s\n", devid,
		di_args[dev_idx].path,
		pretty_size_mode(di_args[dev_idx].total_bytes, UNITS_DEFAULT),
		res_str);

out:
	free(di_args);
	return 0;
}

static int cmd_filesystem_resize(const struct cmd_struct *cmd,
				 int argc, char **argv)
{
	struct btrfs_ioctl_vol_args	args;
	int	fd, res, len, e;
	char	*amount, *path;
	DIR	*dirstream = NULL;
	int ret;
	bool enqueue = false;

	/*
	 * Simplified option parser, accept only long options, the resize value
	 * could be negative and is recognized as short options by getopt
	 */
	for (optind = 1; optind < argc; optind++) {
		if (strcmp(argv[optind], "--enqueue") == 0) {
			enqueue = true;
		} else if (strcmp(argv[optind], "--") == 0) {
			/* Separator: options -- non-options */
		} else if (strncmp(argv[optind], "--", 2) == 0) {
			/* Emulate what getopt does on unkonwn option */
			optind++;
			usage_unknown_option(cmd, argv);
		} else {
			break;
		}
	}

	if (check_argc_exact(argc - optind, 2))
		return 1;

	amount = argv[optind];
	path = argv[optind + 1];

	len = strlen(amount);
	if (len == 0 || len >= BTRFS_VOL_NAME_MAX) {
		error("resize value too long (%s)", amount);
		return 1;
	}

	fd = btrfs_open_dir(path, &dirstream, 1);
	if (fd < 0) {
		/* The path is a directory */
		if (fd == -3) {
			error(
		"resize works on mounted filesystems and accepts only\n"
		"directories as argument. Passing file containing a btrfs image\n"
		"would resize the underlying filesystem instead of the image.\n");
		}
		return 1;
	}

	ret = check_running_fs_exclop(fd, BTRFS_EXCLOP_RESIZE, enqueue);
	if (ret != 0) {
		if (ret < 0)
			error("unable to check status of exclusive operation: %m");
		close_file_or_dir(fd, dirstream);
		return 1;
	}

	ret = check_resize_args(amount, path);
	if (ret != 0) {
		close_file_or_dir(fd, dirstream);
		return 1;
	}

	memset(&args, 0, sizeof(args));
	strncpy_null(args.name, amount);
	res = ioctl(fd, BTRFS_IOC_RESIZE, &args);
	e = errno;
	close_file_or_dir(fd, dirstream);
	if( res < 0 ){
		switch (e) {
		case EFBIG:
			error("unable to resize '%s': no enough free space",
				path);
			break;
		default:
			error("unable to resize '%s': %m", path);
			break;
		}
		return 1;
	} else if (res > 0) {
		const char *err_str = btrfs_err_str(res);

		if (err_str) {
			error("resizing of '%s' failed: %s", path, err_str);
		} else {
			error("resizing of '%s' failed: unknown error %d",
				path, res);
		}
		return 1;
	}
	return 0;
}
static DEFINE_SIMPLE_COMMAND(filesystem_resize, "resize");

static const char * const cmd_filesystem_label_usage[] = {
	"btrfs filesystem label [<device>|<mount_point>] [<newlabel>]",
	"Get or change the label of a filesystem",
	"With one argument, get the label of filesystem on <device>.",
	"If <newlabel> is passed, set the filesystem label to <newlabel>.",
	NULL
};

static int cmd_filesystem_label(const struct cmd_struct *cmd,
				int argc, char **argv)
{
	clean_args_no_options(cmd, argc, argv);

	if (check_argc_min(argc - optind, 1) ||
			check_argc_max(argc - optind, 2))
		return 1;

	if (argc - optind > 1) {
		return set_label(argv[optind], argv[optind + 1]);
	} else {
		char label[BTRFS_LABEL_SIZE];
		int ret;

		ret = get_label(argv[optind], label);
		if (!ret)
			fprintf(stdout, "%s\n", label);

		return ret;
	}
}
static DEFINE_SIMPLE_COMMAND(filesystem_label, "label");

static const char * const cmd_filesystem_balance_usage[] = {
	"btrfs filesystem balance [args...] (alias of \"btrfs balance\")",
	"Please see \"btrfs balance --help\" for more information.",
	NULL
};

static int cmd_filesystem_balance(const struct cmd_struct *unused,
				  int argc, char **argv)
{
	return cmd_execute(&cmd_struct_balance, argc, argv);
}

/*
 * Compatible old "btrfs filesystem balance" command
 *
 * We can't use cmd_struct_balance directly here since this alias is
 * for historical compatibility and is hidden.
 */
static DEFINE_COMMAND(filesystem_balance, "balance", cmd_filesystem_balance,
		      cmd_filesystem_balance_usage, NULL, CMD_HIDDEN);

static const char filesystem_cmd_group_info[] =
"overall filesystem tasks and information";

static const struct cmd_group filesystem_cmd_group = {
	filesystem_cmd_group_usage, filesystem_cmd_group_info, {
		&cmd_struct_filesystem_df,
		&cmd_struct_filesystem_du,
		&cmd_struct_filesystem_show,
		&cmd_struct_filesystem_sync,
		&cmd_struct_filesystem_defrag,
		&cmd_struct_filesystem_balance,
		&cmd_struct_filesystem_resize,
		&cmd_struct_filesystem_label,
		&cmd_struct_filesystem_usage,
		NULL
	}
};

DEFINE_GROUP_COMMAND_TOKEN(filesystem);
