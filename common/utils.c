/*
 * Copyright (C) 2007 Oracle.  All rights reserved.
 * Copyright (C) 2008 Morey Roof.  All rights reserved.
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
#include <sys/ioctl.h>
#include <sys/mount.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <sys/sysinfo.h>
#include <uuid/uuid.h>
#include <fcntl.h>
#include <unistd.h>
#include <mntent.h>
#include <ctype.h>
#include <limits.h>
#include <blkid/blkid.h>
#include <sys/vfs.h>
#include <sys/statfs.h>
#include <linux/magic.h>
#include <getopt.h>

#include <btrfsutil.h>

#include "kerncompat.h"
#include "kernel-lib/radix-tree.h"
#include "kernel-shared/ctree.h"
#include "kernel-shared/disk-io.h"
#include "kernel-shared/transaction.h"
#include "crypto/crc32c.h"
#include "common/utils.h"
#include "common/path-utils.h"
#include "common/device-scan.h"
#include "kernel-shared/volumes.h"
#include "ioctl.h"
#include "cmds/commands.h"
#include "common/open-utils.h"
#include "mkfs/common.h"

static int rand_seed_initialized = 0;
static unsigned short rand_seed[3];

struct btrfs_config bconf;

struct pending_dir {
	struct list_head list;
	char name[PATH_MAX];
};

/*
 * Checks to make sure that the label matches our requirements.
 * Returns:
       0    if everything is safe and usable
      -1    if the label is too long
 */
static int check_label(const char *input)
{
       int len = strlen(input);

       if (len > BTRFS_LABEL_SIZE - 1) {
		error("label %s is too long (max %d)", input,
				BTRFS_LABEL_SIZE - 1);
               return -1;
       }

       return 0;
}

static int set_label_unmounted(const char *dev, const char *label)
{
	struct btrfs_trans_handle *trans;
	struct btrfs_root *root;
	int ret;

	ret = check_mounted(dev);
	if (ret < 0) {
	       error("checking mount status of %s failed: %d", dev, ret);
	       return -1;
	}
	if (ret > 0) {
		error("device %s is mounted, use mount point", dev);
		return -1;
	}

	/* Open the super_block at the default location
	 * and as read-write.
	 */
	root = open_ctree(dev, 0, OPEN_CTREE_WRITES);
	if (!root) /* errors are printed by open_ctree() */
		return -1;

	trans = btrfs_start_transaction(root, 1);
	BUG_ON(IS_ERR(trans));
	__strncpy_null(root->fs_info->super_copy->label, label, BTRFS_LABEL_SIZE - 1);

	btrfs_commit_transaction(trans, root);

	/* Now we close it since we are done. */
	close_ctree(root);
	return 0;
}

static int set_label_mounted(const char *mount_path, const char *labelp)
{
	int fd;
	char label[BTRFS_LABEL_SIZE];

	fd = open(mount_path, O_RDONLY | O_NOATIME);
	if (fd < 0) {
		error("unable to access %s: %m", mount_path);
		return -1;
	}

	memset(label, 0, sizeof(label));
	__strncpy_null(label, labelp, BTRFS_LABEL_SIZE - 1);
	if (ioctl(fd, BTRFS_IOC_SET_FSLABEL, label) < 0) {
		error("unable to set label of %s: %m", mount_path);
		close(fd);
		return -1;
	}

	close(fd);
	return 0;
}

int get_label_unmounted(const char *dev, char *label)
{
	struct btrfs_root *root;
	int ret;

	ret = check_mounted(dev);
	if (ret < 0) {
	       error("checking mount status of %s failed: %d", dev, ret);
	       return -1;
	}

	/* Open the super_block at the default location
	 * and as read-only.
	 */
	root = open_ctree(dev, 0, 0);
	if(!root)
		return -1;

	__strncpy_null(label, root->fs_info->super_copy->label,
			BTRFS_LABEL_SIZE - 1);

	/* Now we close it since we are done. */
	close_ctree(root);
	return 0;
}

/*
 * If a partition is mounted, try to get the filesystem label via its
 * mounted path rather than device.  Return the corresponding error
 * the user specified the device path.
 */
int get_label_mounted(const char *mount_path, char *labelp)
{
	char label[BTRFS_LABEL_SIZE];
	int fd;
	int ret;

	fd = open(mount_path, O_RDONLY | O_NOATIME);
	if (fd < 0) {
		error("unable to access %s: %m", mount_path);
		return -1;
	}

	memset(label, '\0', sizeof(label));
	ret = ioctl(fd, BTRFS_IOC_GET_FSLABEL, label);
	if (ret < 0) {
		if (errno != ENOTTY)
			error("unable to get label of %s: %m", mount_path);
		ret = -errno;
		close(fd);
		return ret;
	}

	__strncpy_null(labelp, label, BTRFS_LABEL_SIZE - 1);
	close(fd);
	return 0;
}

int get_label(const char *btrfs_dev, char *label)
{
	int ret;

	ret = path_is_reg_or_block_device(btrfs_dev);
	if (!ret)
		ret = get_label_mounted(btrfs_dev, label);
	else if (ret > 0)
		ret = get_label_unmounted(btrfs_dev, label);

	return ret;
}

int set_label(const char *btrfs_dev, const char *label)
{
	int ret;

	if (check_label(label))
		return -1;

	ret = path_is_reg_or_block_device(btrfs_dev);
	if (!ret)
		ret = set_label_mounted(btrfs_dev, label);
	else if (ret > 0)
		ret = set_label_unmounted(btrfs_dev, label);

	return ret;
}

/*
 * A not-so-good version fls64. No fascinating optimization since
 * no one except parse_size_from_string uses it
 */
static int fls64(u64 x)
{
	int i;

	for (i = 0; i <64; i++)
		if (x << i & (1ULL << 63))
			return 64 - i;
	return 64 - i;
}

u64 parse_size_from_string(const char *s)
{
	char c;
	char *endptr;
	u64 mult = 1;
	u64 ret;

	if (!s) {
		error("size value is empty");
		exit(1);
	}
	if (s[0] == '-') {
		error("size value '%s' is less equal than 0", s);
		exit(1);
	}
	ret = strtoull(s, &endptr, 10);
	if (endptr == s) {
		error("size value '%s' is invalid", s);
		exit(1);
	}
	if (endptr[0] && endptr[1]) {
		error("illegal suffix contains character '%c' in wrong position",
			endptr[1]);
		exit(1);
	}
	/*
	 * strtoll returns LLONG_MAX when overflow, if this happens,
	 * need to call strtoull to get the real size
	 */
	if (errno == ERANGE && ret == ULLONG_MAX) {
		error("size value '%s' is too large for u64", s);
		exit(1);
	}
	if (endptr[0]) {
		c = tolower(endptr[0]);
		switch (c) {
		case 'e':
			mult *= 1024;
			/* fallthrough */
		case 'p':
			mult *= 1024;
			/* fallthrough */
		case 't':
			mult *= 1024;
			/* fallthrough */
		case 'g':
			mult *= 1024;
			/* fallthrough */
		case 'm':
			mult *= 1024;
			/* fallthrough */
		case 'k':
			mult *= 1024;
			/* fallthrough */
		case 'b':
			break;
		default:
			error("unknown size descriptor '%c'", c);
			exit(1);
		}
	}
	/* Check whether ret * mult overflow */
	if (fls64(ret) + fls64(mult) - 1 > 64) {
		error("size value '%s' is too large for u64", s);
		exit(1);
	}
	ret *= mult;
	return ret;
}

u64 parse_qgroupid(const char *p)
{
	char *s = strchr(p, '/');
	const char *ptr_src_end = p + strlen(p);
	char *ptr_parse_end = NULL;
	enum btrfs_util_error err;
	u64 level;
	u64 id;
	int fd;
	int ret = 0;

	if (p[0] == '/')
		goto path;

	/* Numeric format like '0/257' is the primary case */
	if (!s) {
		id = strtoull(p, &ptr_parse_end, 10);
		if (ptr_parse_end != ptr_src_end)
			goto path;
		return id;
	}
	level = strtoull(p, &ptr_parse_end, 10);
	if (ptr_parse_end != s)
		goto path;

	id = strtoull(s + 1, &ptr_parse_end, 10);
	if (ptr_parse_end != ptr_src_end)
		goto  path;

	return (level << BTRFS_QGROUP_LEVEL_SHIFT) | id;

path:
	/* Path format like subv at 'my_subvol' is the fallback case */
	err = btrfs_util_is_subvolume(p);
	if (err)
		goto err;
	fd = open(p, O_RDONLY);
	if (fd < 0)
		goto err;
	ret = lookup_path_rootid(fd, &id);
	if (ret) {
		errno = -ret;
		error("failed to lookup root id: %m");
	}
	close(fd);
	if (ret < 0)
		goto err;
	return id;

err:
	error("invalid qgroupid or subvolume path: %s", p);
	exit(-1);
}

enum btrfs_csum_type parse_csum_type(const char *s)
{
	if (strcasecmp(s, "crc32c") == 0) {
		return BTRFS_CSUM_TYPE_CRC32;
	} else if (strcasecmp(s, "xxhash64") == 0 ||
		   strcasecmp(s, "xxhash") == 0) {
		return BTRFS_CSUM_TYPE_XXHASH;
	} else if (strcasecmp(s, "sha256") == 0) {
		return BTRFS_CSUM_TYPE_SHA256;
	} else if (strcasecmp(s, "blake2b") == 0 ||
		   strcasecmp(s, "blake2") == 0) {
		return BTRFS_CSUM_TYPE_BLAKE2;
	} else {
		error("unknown csum type %s", s);
		exit(1);
	}
	/* not reached */
	return 0;
}

int get_device_info(int fd, u64 devid,
		struct btrfs_ioctl_dev_info_args *di_args)
{
	int ret;

	di_args->devid = devid;
	memset(&di_args->uuid, '\0', sizeof(di_args->uuid));

	ret = ioctl(fd, BTRFS_IOC_DEV_INFO, di_args);
	return ret < 0 ? -errno : 0;
}

int get_df(int fd, struct btrfs_ioctl_space_args **sargs_ret)
{
	u64 count = 0;
	int ret;
	struct btrfs_ioctl_space_args *sargs;

	sargs = malloc(sizeof(struct btrfs_ioctl_space_args));
	if (!sargs)
		return -ENOMEM;

	sargs->space_slots = 0;
	sargs->total_spaces = 0;

	ret = ioctl(fd, BTRFS_IOC_SPACE_INFO, sargs);
	if (ret < 0) {
		error("cannot get space info: %m");
		free(sargs);
		return -errno;
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
		return -ENOMEM;

	sargs->space_slots = count;
	sargs->total_spaces = 0;
	ret = ioctl(fd, BTRFS_IOC_SPACE_INFO, sargs);
	if (ret < 0) {
		error("cannot get space info with %llu slots: %m",
				count);
		free(sargs);
		return -errno;
	}
	*sargs_ret = sargs;
	return 0;
}


static u64 find_max_device_id(struct btrfs_ioctl_search_args *search_args,
			      int nr_items)
{
	struct btrfs_dev_item *dev_item;
	char *buf = search_args->buf;

	buf += (nr_items - 1) * (sizeof(struct btrfs_ioctl_search_header)
				       + sizeof(struct btrfs_dev_item));
	buf += sizeof(struct btrfs_ioctl_search_header);

	dev_item = (struct btrfs_dev_item *)buf;

	return btrfs_stack_device_id(dev_item);
}

static int search_chunk_tree_for_fs_info(int fd,
				struct btrfs_ioctl_fs_info_args *fi_args)
{
	int ret;
	int max_items;
	u64 start_devid = 1;
	struct btrfs_ioctl_search_args search_args;
	struct btrfs_ioctl_search_key *search_key = &search_args.key;

	fi_args->num_devices = 0;

	max_items = BTRFS_SEARCH_ARGS_BUFSIZE
	       / (sizeof(struct btrfs_ioctl_search_header)
			       + sizeof(struct btrfs_dev_item));

	search_key->tree_id = BTRFS_CHUNK_TREE_OBJECTID;
	search_key->min_objectid = BTRFS_DEV_ITEMS_OBJECTID;
	search_key->max_objectid = BTRFS_DEV_ITEMS_OBJECTID;
	search_key->min_type = BTRFS_DEV_ITEM_KEY;
	search_key->max_type = BTRFS_DEV_ITEM_KEY;
	search_key->min_transid = 0;
	search_key->max_transid = (u64)-1;
	search_key->nr_items = max_items;
	search_key->max_offset = (u64)-1;

again:
	search_key->min_offset = start_devid;

	ret = ioctl(fd, BTRFS_IOC_TREE_SEARCH, &search_args);
	if (ret < 0)
		return -errno;

	fi_args->num_devices += (u64)search_key->nr_items;

	if (search_key->nr_items == max_items) {
		start_devid = find_max_device_id(&search_args,
					search_key->nr_items) + 1;
		goto again;
	}

	/* Get the latest max_id to stay consistent with the num_devices */
	if (search_key->nr_items == 0)
		/*
		 * last tree_search returns an empty buf, use the devid of
		 * the last dev_item of the previous tree_search
		 */
		fi_args->max_id = start_devid - 1;
	else
		fi_args->max_id = find_max_device_id(&search_args,
						search_key->nr_items);

	return 0;
}

/*
 * For a given path, fill in the ioctl fs_ and info_ args.
 * If the path is a btrfs mountpoint, fill info for all devices.
 * If the path is a btrfs device, fill in only that device.
 *
 * The path provided must be either on a mounted btrfs fs,
 * or be a mounted btrfs device.
 *
 * Returns 0 on success, or a negative errno.
 */
int get_fs_info(const char *path, struct btrfs_ioctl_fs_info_args *fi_args,
		struct btrfs_ioctl_dev_info_args **di_ret)
{
	int fd = -1;
	int ret = 0;
	int ndevs = 0;
	u64 last_devid = 0;
	int replacing = 0;
	struct btrfs_fs_devices *fs_devices_mnt = NULL;
	struct btrfs_ioctl_dev_info_args *di_args;
	struct btrfs_ioctl_dev_info_args tmp;
	char mp[PATH_MAX];
	DIR *dirstream = NULL;

	memset(fi_args, 0, sizeof(*fi_args));

	if (path_is_block_device(path) == 1) {
		struct btrfs_super_block *disk_super;
		char buf[BTRFS_SUPER_INFO_SIZE];

		/* Ensure it's mounted, then set path to the mountpoint */
		fd = open(path, O_RDONLY);
		if (fd < 0) {
			ret = -errno;
			error("cannot open %s: %m", path);
			goto out;
		}
		ret = check_mounted_where(fd, path, mp, sizeof(mp),
					  &fs_devices_mnt, SBREAD_DEFAULT);
		if (!ret) {
			ret = -EINVAL;
			goto out;
		}
		if (ret < 0)
			goto out;
		path = mp;
		/* Only fill in this one device */
		fi_args->num_devices = 1;

		disk_super = (struct btrfs_super_block *)buf;
		ret = btrfs_read_dev_super(fd, disk_super,
					   BTRFS_SUPER_INFO_OFFSET, 0);
		if (ret < 0) {
			ret = -EIO;
			goto out;
		}
		last_devid = btrfs_stack_device_id(&disk_super->dev_item);
		fi_args->max_id = last_devid;

		memcpy(fi_args->fsid, fs_devices_mnt->fsid, BTRFS_FSID_SIZE);
		close(fd);
	}

	/* at this point path must not be for a block device */
	fd = open_file_or_dir(path, &dirstream);
	if (fd < 0) {
		ret = -errno;
		goto out;
	}

	/* fill in fi_args if not just a single device */
	if (fi_args->num_devices != 1) {
		ret = ioctl(fd, BTRFS_IOC_FS_INFO, fi_args);
		if (ret < 0) {
			ret = -errno;
			goto out;
		}

		/*
		 * The fs_args->num_devices does not include seed devices
		 */
		ret = search_chunk_tree_for_fs_info(fd, fi_args);
		if (ret)
			goto out;

		/*
		 * search_chunk_tree_for_fs_info() will lacks the devid 0
		 * so manual probe for it here.
		 */
		ret = get_device_info(fd, 0, &tmp);
		if (!ret) {
			fi_args->num_devices++;
			ndevs++;
			replacing = 1;
			if (last_devid == 0)
				last_devid++;
		}
	}

	if (!fi_args->num_devices)
		goto out;

	di_args = *di_ret = malloc((fi_args->num_devices) * sizeof(*di_args));
	if (!di_args) {
		ret = -errno;
		goto out;
	}

	if (replacing)
		memcpy(di_args, &tmp, sizeof(tmp));
	for (; last_devid <= fi_args->max_id && ndevs < fi_args->num_devices;
	     last_devid++) {
		ret = get_device_info(fd, last_devid, &di_args[ndevs]);
		if (ret == -ENODEV)
			continue;
		if (ret)
			goto out;
		ndevs++;
	}

	/*
	* only when the only dev we wanted to find is not there then
	* let any error be returned
	*/
	if (fi_args->num_devices != 1) {
		BUG_ON(ndevs == 0);
		ret = 0;
	}

out:
	close_file_or_dir(fd, dirstream);
	return ret;
}

int get_fsid_fd(int fd, u8 *fsid)
{
	int ret;
	struct btrfs_ioctl_fs_info_args args;

	ret = ioctl(fd, BTRFS_IOC_FS_INFO, &args);
	if (ret < 0)
		return -errno;

	memcpy(fsid, args.fsid, BTRFS_FSID_SIZE);
	return 0;
}

int get_fsid(const char *path, u8 *fsid, int silent)
{
	int ret;
	int fd;

	fd = open(path, O_RDONLY);
	if (fd < 0) {
		if (!silent)
			error("failed to open %s: %m", path);
		return -errno;
	}

	ret = get_fsid_fd(fd, fsid);
	close(fd);

	return ret;
}

static int group_profile_devs_min(u64 flag)
{
	switch (flag & BTRFS_BLOCK_GROUP_PROFILE_MASK) {
	case 0: /* single */
	case BTRFS_BLOCK_GROUP_DUP:
		return 1;
	case BTRFS_BLOCK_GROUP_RAID0:
	case BTRFS_BLOCK_GROUP_RAID1:
	case BTRFS_BLOCK_GROUP_RAID5:
		return 2;
	case BTRFS_BLOCK_GROUP_RAID6:
	case BTRFS_BLOCK_GROUP_RAID1C3:
		return 3;
	case BTRFS_BLOCK_GROUP_RAID10:
	case BTRFS_BLOCK_GROUP_RAID1C4:
		return 4;
	default:
		return -1;
	}
}

int test_num_disk_vs_raid(u64 metadata_profile, u64 data_profile,
	u64 dev_cnt, int mixed, int ssd)
{
	u64 allowed = 0;
	u64 profile = metadata_profile | data_profile;

	switch (dev_cnt) {
	default:
	case 4:
		allowed |= BTRFS_BLOCK_GROUP_RAID10;
		allowed |= BTRFS_BLOCK_GROUP_RAID10 | BTRFS_BLOCK_GROUP_RAID1C4;
		/* fallthrough */
	case 3:
		allowed |= BTRFS_BLOCK_GROUP_RAID6 | BTRFS_BLOCK_GROUP_RAID1C3;
		/* fallthrough */
	case 2:
		allowed |= BTRFS_BLOCK_GROUP_RAID0 | BTRFS_BLOCK_GROUP_RAID1 |
			BTRFS_BLOCK_GROUP_RAID5;
		/* fallthrough */
	case 1:
		allowed |= BTRFS_BLOCK_GROUP_DUP;
	}

	if (dev_cnt > 1 && profile & BTRFS_BLOCK_GROUP_DUP) {
		warning("DUP is not recommended on filesystem with multiple devices");
	}
	if (metadata_profile & ~allowed) {
		fprintf(stderr,
			"ERROR: unable to create FS with metadata profile %s "
			"(have %llu devices but %d devices are required)\n",
			btrfs_group_profile_str(metadata_profile), dev_cnt,
			group_profile_devs_min(metadata_profile));
		return 1;
	}
	if (data_profile & ~allowed) {
		fprintf(stderr,
			"ERROR: unable to create FS with data profile %s "
			"(have %llu devices but %d devices are required)\n",
			btrfs_group_profile_str(data_profile), dev_cnt,
			group_profile_devs_min(data_profile));
		return 1;
	}

	if (dev_cnt == 3 && profile & BTRFS_BLOCK_GROUP_RAID6) {
		warning("RAID6 is not recommended on filesystem with 3 devices only");
	}
	if (dev_cnt == 2 && profile & BTRFS_BLOCK_GROUP_RAID5) {
		warning("RAID5 is not recommended on filesystem with 2 devices only");
	}
	warning_on(!mixed && (data_profile & BTRFS_BLOCK_GROUP_DUP) && ssd,
		   "DUP may not actually lead to 2 copies on the device, see manual page");

	return 0;
}

int group_profile_max_safe_loss(u64 flags)
{
	switch (flags & BTRFS_BLOCK_GROUP_PROFILE_MASK) {
	case 0: /* single */
	case BTRFS_BLOCK_GROUP_DUP:
	case BTRFS_BLOCK_GROUP_RAID0:
		return 0;
	case BTRFS_BLOCK_GROUP_RAID1:
	case BTRFS_BLOCK_GROUP_RAID5:
	case BTRFS_BLOCK_GROUP_RAID10:
		return 1;
	case BTRFS_BLOCK_GROUP_RAID6:
	case BTRFS_BLOCK_GROUP_RAID1C3:
		return 2;
	case BTRFS_BLOCK_GROUP_RAID1C4:
		return 3;
	default:
		return -1;
	}
}

/*
 * This reads a line from the stdin and only returns non-zero if the
 * first whitespace delimited token is a case insensitive match with yes
 * or y.
 */
int ask_user(const char *question)
{
	char buf[30] = {0,};
	char *saveptr = NULL;
	char *answer;

	printf("%s [y/N]: ", question);

	return fgets(buf, sizeof(buf) - 1, stdin) &&
	       (answer = strtok_r(buf, " \t\n\r", &saveptr)) &&
	       (!strcasecmp(answer, "yes") || !strcasecmp(answer, "y"));
}

/*
 * Partial representation of a line in /proc/pid/mountinfo
 */
struct mnt_entry {
	const char *root;
	const char *path;
	const char *options1;
	const char *fstype;
	const char *device;
	const char *options2;
};

/*
 * Find first occurence of up an option string (as "option=") in @options,
 * separated by comma. Return allocated string as "option=value"
 */
static char *find_option(const char *options, const char *option)
{
	char *tmp, *ret;

	tmp = strstr(options, option);
	if (!tmp)
		return NULL;
	ret = strdup(tmp);
	tmp = ret;
	while (*tmp && *tmp != ',')
		tmp++;
	*tmp = 0;
	return ret;
}

/* Match whitespace separator */
static bool is_sep(char c)
{
	return c == ' ' || c == '\t';
}

/* Advance @line skipping over all non-separator chars */
static void skip_nonsep(char **line)
{
	while (**line && !is_sep(**line))
		(*line)++;
}

/* Advance @line skipping over all separator chars, setting them to nul char */
static void skip_sep(char **line)
{
	while (**line && is_sep(**line)) {
		**line = 0;
		(*line)++;
	}
}

static bool isoctal(char c)
{
	return '0' <= c && c <= '7';
}

/*
 * Validate complete escape sequence used for mangling special chars in paths,
 * eg.  \012 == 10 == 0xa == '\n'.
 * Mandatory format: backslash and 3 octal digits.
 */
static bool valid_escape(const char *str)
{
	if (*str == 0 || *str != '\\')
		return false;
	str++;
	if (*str == 0 || is_sep(*str) || !isoctal(*str))
		return false;
	str++;
	if (*str == 0 || is_sep(*str) || !isoctal(*str))
		return false;
	str++;
	if (*str == 0 || is_sep(*str) || !isoctal(*str))
		return false;
	return true;
}

/*
 * Read a path from @line, with potentially mangled special characters.
 * - the input is changed in-place when unmangling is done
 * - end of path is a space character (a valid space in the path is mangled)
 * - line is advanced to the final separator or nul character
 * - returned path is a valid string terminated by zero or whitespace separator
 */
char *read_path(char **line)
{
	char *ret = *line;
	char *out = *line;

	while (**line) {
		if (is_sep(**line))
			break;
		if (valid_escape(*line)) {
			char c;

			(*line)++;
			c  = ((*(*line)++) & 0b111) << 6;
			c |= ((*(*line)++) & 0b111) << 3;
			c |= ((*(*line)++) & 0b111);
			*out++ = c;
		} else {
			*out++ = *(*line)++;
		}
	}
	/*
	 * Unmangled characters make the final string shorter, add the null
	 * terminator.  Otherwise keep the line at the space separator so
	 * followup parsing can continue.
	 */
	if (out < *line)
		*out = 0;
	return ret;
}

/*
 * Parse a line from /proc/pid/mountinfo
 * Example:

272 265 0:49 /subvol /mnt/path rw,noatime shared:145 - btrfs /dev/sda1 rw,subvolid=5598,subvol=/subvol
0   1   2    3      4          5          6          7 8     9         10

 * Fields related to paths and options are parsed, @line is changed in place,
 * separators are replaced by nul char, paths could be unmangled.
 */
static void parse_mntinfo_line(char *line, struct mnt_entry *ent)
{
	/* Skip 0 */
	skip_nonsep(&line);
	skip_sep(&line);
	/* Skip 1 */
	skip_nonsep(&line);
	skip_sep(&line);
	/* Skip 2 */
	skip_nonsep(&line);
	skip_sep(&line);
	/* Read 3 */
	ent->root = read_path(&line);
	skip_sep(&line);
	/* Read 4 */
	ent->path = read_path(&line);
	skip_sep(&line);
	/* Read 5 */
	ent->options1 = line;
	skip_nonsep(&line);
	skip_sep(&line);
	/* Skip 6 */
	skip_nonsep(&line);
	skip_sep(&line);
	/* Skip 7 */
	skip_nonsep(&line);
	skip_sep(&line);
	/* Read 8 */
	ent->fstype = line;
	skip_nonsep(&line);
	skip_sep(&line);
	/* Read 9 */
	ent->device = read_path(&line);
	skip_sep(&line);
	/* Read 10 */
	ent->options2 = line;
	skip_nonsep(&line);
	skip_sep(&line);
}

/*
 * Compare the subvolume passed with the pathname of the directory mounted in
 * btrfs. The pathname inside btrfs is different from getmnt and friends, since
 * it can detect bind mounts to content from the inside of the original mount.
 *
 * Example:
 *   # mount -o subvol=/vol /dev/sda2 /mnt
 *   # mount --bind /mnt/dir2 /othermnt
 *
 *   # mounts
 *   ...
 *   /dev/sda2 on /mnt type btrfs (ro,relatime,ssd,space_cache,subvolid=256,subvol=/vol)
 *   /dev/sda2 on /othermnt type btrfs (ro,relatime,ssd,space_cache,subvolid=256,subvol=/vol)
 *
 *   # cat /proc/self/mountinfo
 *
 *   38 30 0:32 /vol /mnt ro,relatime - btrfs /dev/sda2 ro,ssd,space_cache,subvolid=256,subvol=/vol
 *   37 29 0:32 /vol/dir2 /othermnt ro,relatime - btrfs /dev/sda2 ro,ssd,space_cache,subvolid=256,subvol=/vol
 *
 * If we try to find a mount point only using subvol and subvolid from mount
 * options we would get mislead to belive that /othermnt has the same content
 * as /mnt.
 *
 * But, using mountinfo, we have the pathaname _inside_ the filesystem, so we
 * can filter out the mount points with bind mounts which have different content
 * from the original mounts, in this case the mount point with id 37.
 */
int find_mount_fsroot(const char *subvol, const char *subvolid, char **mount)
{
	FILE *mnt;
	char *buf = NULL;
	int bs = 4096;
	int line = 0;
	int ret = 0;
	bool found = false;

	mnt = fopen("/proc/self/mountinfo", "r");
	if (!mnt)
		return -1;

	buf = malloc(bs);
	if (!buf) {
		ret = -ENOMEM;
		goto out;
	}

	do {
		int ch;

		ch = fgetc(mnt);
		if (ch == -1)
			break;

		if (ch == '\n') {
			struct mnt_entry ent;
			char *opt;
			const char *value;

			buf[line] = 0;
			parse_mntinfo_line(buf, &ent);

			/* Skip unrelated mounts */
			if (strcmp(ent.fstype, "btrfs") != 0)
				goto nextline;
			if (strlen(ent.root) != strlen(subvol))
				goto nextline;
			if (strcmp(ent.root, subvol) != 0)
				goto nextline;

			/*
			 * Match subvolume by id found in mountinfo and
			 * requested by the caller
			 */
			opt = find_option(ent.options2, "subvolid=");
			if (!opt)
				goto nextline;
			value = opt + strlen("subvolid=");
			if (strcmp(value, subvolid) != 0) {
				free(opt);
				goto nextline;
			}
			free(opt);

			/*
			 * First match is in most cases the original mount, not
			 * a bind mount. In case there are no further bind
			 * mounts, return what we found in @mount.  Any
			 * following mount that matches by path and subvolume
			 * id is a bind mount and we return the original mount.
			 */
			if (found)
				goto out;
			found = true;
			*mount = strdup(ent.path);
			ret = 0;
			goto nextline;
		}
		/*
		 * Grow buffer if needed, there are 3 paths up to PATH_MAX and
		 * mount options are limited by page size. Often the overall
		 * line length does not exceed 256.
		 */
		if (line >= bs) {
			char *tmp;

			bs += 4096;
			tmp = realloc(buf, bs);
			if (!tmp) {
				ret = -ENOMEM;
				goto out;
			}
			buf = tmp;
		}
		buf[line++] = ch;
		continue;
nextline:
		line = 0;
	} while (1);
out:
	free(buf);
	fclose(mnt);
	return ret;
}

/*
 * return 0 if a btrfs mount point is found
 * return 1 if a mount point is found but not btrfs
 * return <0 if something goes wrong
 */
int find_mount_root(const char *path, char **mount_root)
{
	FILE *mnttab;
	int fd;
	struct mntent *ent;
	int len;
	int ret = 0;
	int not_btrfs = 1;
	int longest_matchlen = 0;
	char *longest_match = NULL;

	fd = open(path, O_RDONLY | O_NOATIME);
	if (fd < 0)
		return -errno;
	close(fd);

	mnttab = setmntent("/proc/self/mounts", "r");
	if (!mnttab)
		return -errno;

	while ((ent = getmntent(mnttab))) {
		if (path_is_in_dir(ent->mnt_dir, path)) {
			len = strlen(ent->mnt_dir);
			if (longest_matchlen <= len) {
				free(longest_match);
				longest_matchlen = len;
				longest_match = strdup(ent->mnt_dir);
				if (!longest_match) {
					ret = -errno;
					break;
				}
				not_btrfs = strcmp(ent->mnt_type, "btrfs");
			}
		}
	}
	endmntent(mnttab);

	if (ret)
		return ret;
	if (!longest_match)
		return -ENOENT;
	if (not_btrfs) {
		free(longest_match);
		return 1;
	}

	ret = 0;
	*mount_root = realpath(longest_match, NULL);
	if (!*mount_root)
		ret = -errno;

	free(longest_match);
	return ret;
}

int find_next_key(struct btrfs_path *path, struct btrfs_key *key)
{
	int level;

	for (level = 0; level < BTRFS_MAX_LEVEL; level++) {
		if (!path->nodes[level])
			break;
		if (path->slots[level] + 1 >=
		    btrfs_header_nritems(path->nodes[level]))
			continue;
		if (level == 0)
			btrfs_item_key_to_cpu(path->nodes[level], key,
					      path->slots[level] + 1);
		else
			btrfs_node_key_to_cpu(path->nodes[level], key,
					      path->slots[level] + 1);
		return 0;
	}
	return 1;
}

const char* btrfs_group_type_str(u64 flag)
{
	u64 mask = BTRFS_BLOCK_GROUP_TYPE_MASK |
		BTRFS_SPACE_INFO_GLOBAL_RSV;

	switch (flag & mask) {
	case BTRFS_BLOCK_GROUP_DATA:
		return "Data";
	case BTRFS_BLOCK_GROUP_SYSTEM:
		return "System";
	case BTRFS_BLOCK_GROUP_METADATA:
		return "Metadata";
	case BTRFS_BLOCK_GROUP_DATA|BTRFS_BLOCK_GROUP_METADATA:
		return "Data+Metadata";
	case BTRFS_SPACE_INFO_GLOBAL_RSV:
		return "GlobalReserve";
	default:
		return "unknown";
	}
}

const char* btrfs_group_profile_str(u64 flag)
{
	switch (flag & BTRFS_BLOCK_GROUP_PROFILE_MASK) {
	case 0:
		return "single";
	case BTRFS_BLOCK_GROUP_RAID0:
		return "RAID0";
	case BTRFS_BLOCK_GROUP_RAID1:
		return "RAID1";
	case BTRFS_BLOCK_GROUP_RAID1C3:
		return "RAID1C3";
	case BTRFS_BLOCK_GROUP_RAID1C4:
		return "RAID1C4";
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
/*
 * Check if the BTRFS_IOC_TREE_SEARCH_V2 ioctl is supported on a given
 * filesystem, opened at fd
 */
int btrfs_tree_search2_ioctl_supported(int fd)
{
	struct btrfs_ioctl_search_args_v2 *args2;
	struct btrfs_ioctl_search_key *sk;
	int args2_size = 1024;
	char args2_buf[args2_size];
	int ret;

	args2 = (struct btrfs_ioctl_search_args_v2 *)args2_buf;
	sk = &(args2->key);

	/*
	 * Search for the extent tree item in the root tree.
	 */
	sk->tree_id = BTRFS_ROOT_TREE_OBJECTID;
	sk->min_objectid = BTRFS_EXTENT_TREE_OBJECTID;
	sk->max_objectid = BTRFS_EXTENT_TREE_OBJECTID;
	sk->min_type = BTRFS_ROOT_ITEM_KEY;
	sk->max_type = BTRFS_ROOT_ITEM_KEY;
	sk->min_offset = 0;
	sk->max_offset = (u64)-1;
	sk->min_transid = 0;
	sk->max_transid = (u64)-1;
	sk->nr_items = 1;
	args2->buf_size = args2_size - sizeof(struct btrfs_ioctl_search_args_v2);
	ret = ioctl(fd, BTRFS_IOC_TREE_SEARCH_V2, args2);
	if (ret == -EOPNOTSUPP)
		return 0;
	else if (ret == 0)
		return 1;
	return ret;
}

u64 div_factor(u64 num, int factor)
{
	if (factor == 10)
		return num;
	num *= factor;
	num /= 10;
	return num;
}
/*
 * Get the length of the string converted from a u64 number.
 *
 * Result is equal to log10(num) + 1, but without the use of math library.
 */
int count_digits(u64 num)
{
	int ret = 0;

	if (num == 0)
		return 1;
	while (num > 0) {
		ret++;
		num /= 10;
	}
	return ret;
}

int string_is_numerical(const char *str)
{
	if (!str)
		return 0;
	if (!(*str >= '0' && *str <= '9'))
		return 0;
	while (*str >= '0' && *str <= '9')
		str++;
	if (*str != '\0')
		return 0;
	return 1;
}

int prefixcmp(const char *str, const char *prefix)
{
	for (; ; str++, prefix++)
		if (!*prefix)
			return 0;
		else if (*str != *prefix)
			return (unsigned char)*prefix - (unsigned char)*str;
}

const char *subvol_strip_mountpoint(const char *mnt, const char *full_path)
{
	int len = strlen(mnt);
	if (!len)
		return full_path;

	if ((strncmp(mnt, full_path, len) != 0) || ((len > 1) && (full_path[len] != '/'))) {
		error("not on mount point: %s", mnt);
		exit(1);
	}

	if (mnt[len - 1] != '/')
		len += 1;

	return full_path + len;
}

/* Set the seed manually */
void init_rand_seed(u64 seed)
{
	int i;

	/* only use the last 48 bits */
	for (i = 0; i < 3; i++) {
		rand_seed[i] = (unsigned short)(seed ^ (unsigned short)(-1));
		seed >>= 16;
	}
	rand_seed_initialized = 1;
}

static void __init_seed(void)
{
	struct timeval tv;
	int ret;
	int fd;

	if(rand_seed_initialized)
		return;
	/* Use urandom as primary seed source. */
	fd = open("/dev/urandom", O_RDONLY);
	if (fd >= 0) {
		ret = read(fd, rand_seed, sizeof(rand_seed));
		close(fd);
		if (ret < sizeof(rand_seed))
			goto fallback;
	} else {
fallback:
		/* Use time and pid as fallback seed */
		warning("failed to read /dev/urandom, use time and pid as random seed");
		gettimeofday(&tv, 0);
		rand_seed[0] = getpid() ^ (tv.tv_sec & 0xFFFF);
		rand_seed[1] = getppid() ^ (tv.tv_usec & 0xFFFF);
		rand_seed[2] = (tv.tv_sec ^ tv.tv_usec) >> 16;
	}
	rand_seed_initialized = 1;
}

u32 rand_u32(void)
{
	__init_seed();
	/*
	 * Don't use nrand48, its range is [0,2^31) The highest bit will always
	 * be 0.  Use jrand48 to include the highest bit.
	 */
	return (u32)jrand48(rand_seed);
}

/* Return random number in range [0, upper) */
unsigned int rand_range(unsigned int upper)
{
	__init_seed();
	/*
	 * Use the full 48bits to mod, which would be more uniformly
	 * distributed
	 */
	return (unsigned int)(jrand48(rand_seed) % upper);
}

int rand_int(void)
{
	return (int)(rand_u32());
}

u64 rand_u64(void)
{
	u64 ret = 0;

	ret += rand_u32();
	ret <<= 32;
	ret += rand_u32();
	return ret;
}

u16 rand_u16(void)
{
	return (u16)(rand_u32());
}

u8 rand_u8(void)
{
	return (u8)(rand_u32());
}

void btrfs_config_init(void)
{
	bconf.output_format = CMD_FORMAT_TEXT;
	bconf.verbose = BTRFS_BCONF_UNSET;
}

void bconf_be_verbose(void)
{
	if (bconf.verbose == BTRFS_BCONF_UNSET)
		bconf.verbose = 1;
	else
		bconf.verbose++;
}

void bconf_be_quiet(void)
{
	bconf.verbose = BTRFS_BCONF_QUIET;
}

/* Returns total size of main memory in bytes, -1UL if error. */
unsigned long total_memory(void)
{
        struct sysinfo si;

        if (sysinfo(&si) < 0) {
                error("can't determine memory size");
                return -1UL;
        }
        return si.totalram * si.mem_unit;       /* bytes */
}

void print_device_info(struct btrfs_device *device, char *prefix)
{
	if (prefix)
		printf("%s", prefix);
	printf("Device: id = %llu, name = %s\n",
	       device->devid, device->name);
}

void print_all_devices(struct list_head *devices)
{
	struct btrfs_device *dev;

	printf("All Devices:\n");
	list_for_each_entry(dev, devices, dev_list)
		print_device_info(dev, "\t");
	printf("\n");
}

static int bit_count(u64 x)
{
	int ret = 0;

	while (x) {
		if (x & 1)
			ret++;
		x >>= 1;
	}
	return ret;
}

static char *sprint_profiles(u64 profiles)
{
	int i;
	int maxlen = 1;
	char *ptr;

	if (bit_count(profiles) <= 1)
		return NULL;

	for (i = 0; i < BTRFS_NR_RAID_TYPES; i++)
		maxlen += strlen(btrfs_raid_array[i].raid_name) + 2;

	ptr = calloc(1, maxlen);
	if (!ptr)
		return NULL;

	if (profiles & BTRFS_AVAIL_ALLOC_BIT_SINGLE)
		strcat(ptr, btrfs_raid_array[BTRFS_RAID_SINGLE].raid_name);

	for (i = 0; i < BTRFS_NR_RAID_TYPES; i++) {
		if (!(btrfs_raid_array[i].bg_flag & profiles))
			continue;

		if (ptr[0])
			strcat(ptr, ", ");
		strcat(ptr, btrfs_raid_array[i].raid_name);
	}

	return ptr;
}

static int btrfs_get_string_for_multiple_profiles(int fd, char **data_ret,
		char **metadata_ret, char **mixed_ret, char **system_ret,
		char **types_ret)
{
	int ret;
	int i;
	struct btrfs_ioctl_space_args *sargs;
	u64 data_profiles = 0;
	u64 metadata_profiles = 0;
	u64 system_profiles = 0;
	u64 mixed_profiles = 0;
	const u64 mixed_profile_fl = BTRFS_BLOCK_GROUP_METADATA |
		BTRFS_BLOCK_GROUP_DATA;

	ret = get_df(fd, &sargs);
	if (ret < 0)
		return -1;

	for (i = 0; i < sargs->total_spaces; i++) {
		u64 flags = sargs->spaces[i].flags;

		if (!(flags & BTRFS_BLOCK_GROUP_PROFILE_MASK))
			flags |= BTRFS_AVAIL_ALLOC_BIT_SINGLE;

		if ((flags & mixed_profile_fl) == mixed_profile_fl)
			mixed_profiles |= flags;
		else if (flags & BTRFS_BLOCK_GROUP_DATA)
			data_profiles |= flags;
		else if (flags & BTRFS_BLOCK_GROUP_METADATA)
			metadata_profiles |= flags;
		else if (flags & BTRFS_BLOCK_GROUP_SYSTEM)
			system_profiles |= flags;
	}
	free(sargs);

	data_profiles &= BTRFS_EXTENDED_PROFILE_MASK;
	system_profiles &= BTRFS_EXTENDED_PROFILE_MASK;
	mixed_profiles &= BTRFS_EXTENDED_PROFILE_MASK;
	metadata_profiles &= BTRFS_EXTENDED_PROFILE_MASK;

	*data_ret = sprint_profiles(data_profiles);
	*metadata_ret = sprint_profiles(metadata_profiles);
	*mixed_ret = sprint_profiles(mixed_profiles);
	*system_ret = sprint_profiles(system_profiles);

	if (types_ret) {
		*types_ret = calloc(1, 64);
		if (!*types_ret)
			goto out;
		if (*data_ret)
			strcat(*types_ret, "data");
		if (*metadata_ret) {
			if ((*types_ret)[0])
				strcat(*types_ret, ", ");
			strcat(*types_ret, "metadata");
		}
		if (*mixed_ret) {
			if ((*types_ret)[0])
				strcat(*types_ret, ", ");
			strcat(*types_ret, "data+metadata");
		}
		if (*system_ret) {
			if ((*types_ret)[0])
				strcat(*types_ret, ", ");
			strcat(*types_ret, "system");
		}
	}

out:
	return *data_ret || *metadata_ret || *mixed_ret || *system_ret;
}

/*
 * Return string containing coma separated list of block group types that
 * contain multiple profiles. The return value must be freed by the caller.
 */
char *btrfs_test_for_multiple_profiles(int fd)
{
	char *data, *metadata, *system, *mixed, *types;

	btrfs_get_string_for_multiple_profiles(fd, &data, &metadata, &mixed,
			&system, &types);
	free(data);
	free(metadata);
	free(mixed);
	free(system);

	return types;
}

int btrfs_warn_multiple_profiles(int fd)
{
	int ret;
	char *data_prof, *mixed_prof, *metadata_prof, *system_prof;

	ret = btrfs_get_string_for_multiple_profiles(fd, &data_prof,
			&metadata_prof, &mixed_prof, &system_prof, NULL);

	if (ret != 1)
		return ret;

	fprintf(stderr,
		"WARNING: Multiple block group profiles detected, see 'man btrfs(5)'.\n");
	if (data_prof)
		fprintf(stderr, "WARNING:   Data: %s\n", data_prof);

	if (metadata_prof)
		fprintf(stderr, "WARNING:   Metadata: %s\n", metadata_prof);

	if (mixed_prof)
		fprintf(stderr, "WARNING:   Data+Metadata: %s\n", mixed_prof);

	if (system_prof)
		fprintf(stderr, "WARNING:   System: %s\n", system_prof);

	free(data_prof);
	free(metadata_prof);
	free(mixed_prof);
	free(system_prof);

	return 1;
}

/*
 * Open a file in fsid directory in sysfs and return the file descriptor or
 * error
 */
int sysfs_open_fsid_file(int fd, const char *filename)
{
	u8 fsid[BTRFS_UUID_SIZE];
	char fsid_str[BTRFS_UUID_UNPARSED_SIZE];
	char sysfs_file[PATH_MAX];
	int ret;

	ret = get_fsid_fd(fd, fsid);
	if (ret < 0)
		return ret;
	uuid_unparse(fsid, fsid_str);

	ret = path_cat3_out(sysfs_file, "/sys/fs/btrfs", fsid_str, filename);
	if (ret < 0)
		return ret;

	return open(sysfs_file, O_RDONLY);
}

/*
 * Open a file in the toplevel sysfs directory and return the file descriptor
 * or error.
 */
int sysfs_open_file(const char *name)
{
	char path[PATH_MAX];
	int ret;

	ret = path_cat_out(path, "/sys/fs/btrfs", name);
	if (ret < 0)
		return ret;
	return open(path, O_RDONLY);
}

/*
 * Open a directory by name in fsid directory in sysfs and return the file
 * descriptor or error, filedescriptor suitable for fdreaddir. The @dirname
 * must be a directory name.
 */
int sysfs_open_fsid_dir(int fd, const char *dirname)
{
	u8 fsid[BTRFS_UUID_SIZE];
	char fsid_str[BTRFS_UUID_UNPARSED_SIZE];
	char sysfs_file[PATH_MAX];
	int ret;

	ret = get_fsid_fd(fd, fsid);
	if (ret < 0)
		return ret;
	uuid_unparse(fsid, fsid_str);

	ret = path_cat3_out(sysfs_file, "/sys/fs/btrfs", fsid_str, dirname);
	if (ret < 0)
		return ret;

	return open(sysfs_file, O_DIRECTORY | O_RDONLY);
}

/*
 * Read up to @size bytes to @buf from @fd
 */
int sysfs_read_file(int fd, char *buf, size_t size)
{
	lseek(fd, 0, SEEK_SET);
	memset(buf, 0, size);
	return read(fd, buf, size);
}

static const char exclop_def[][16] = {
	[BTRFS_EXCLOP_NONE]		= "none",
	[BTRFS_EXCLOP_BALANCE]		= "balance",
	[BTRFS_EXCLOP_DEV_ADD]		= "device add",
	[BTRFS_EXCLOP_DEV_REMOVE]	= "device remove",
	[BTRFS_EXCLOP_DEV_REPLACE]	= "device replace",
	[BTRFS_EXCLOP_RESIZE]		= "resize",
	[BTRFS_EXCLOP_SWAP_ACTIVATE]	= "swap activate",
};

/*
 * Read currently running exclusive operation from sysfs. If this is not
 * available, return BTRFS_EXCLOP_UNKNOWN
 */
int get_fs_exclop(int fd)
{
	int sysfs_fd;
	char buf[32];
	int ret;
	int i;

	sysfs_fd = sysfs_open_fsid_file(fd, "exclusive_operation");
	if (sysfs_fd < 0)
		return BTRFS_EXCLOP_UNKNOWN;

	memset(buf, 0, sizeof(buf));
	ret = sysfs_read_file(sysfs_fd, buf, sizeof(buf));
	close(sysfs_fd);
	if (ret <= 0)
		return BTRFS_EXCLOP_UNKNOWN;

	i = strlen(buf) - 1;
	while (i > 0 && isspace(buf[i])) i--;
	if (i > 0)
		buf[i + 1] = 0;
	for (i = 0; i < ARRAY_SIZE(exclop_def); i++) {
		if (strcmp(exclop_def[i], buf) == 0)
			return i;
	}

	return BTRFS_EXCLOP_UNKNOWN;
}

const char *get_fs_exclop_name(int op)
{
	if (0 <= op && op <= ARRAY_SIZE(exclop_def))
		return exclop_def[op];
	return "UNKNOWN";
}

/*
 * Check if there's another exclusive operation running and either return error
 * or wait until there's none in case @enqueue is true. The timeout between
 * checks is 1 minute as we get notification on the sysfs file when the
 * operation finishes.
 *
 * Return:
 * 0  - caller can continue, nothing running or the status is not available
 * 1  - another operation running
 * <0 - there was another error
 */
int check_running_fs_exclop(int fd, enum exclusive_operation start, bool enqueue)
{
	int sysfs_fd;
	int exclop;
	int ret;

	sysfs_fd = sysfs_open_fsid_file(fd, "exclusive_operation");
	if (sysfs_fd < 0) {
		if (errno == ENOENT)
			return 0;
		return -errno;
	}

	exclop = get_fs_exclop(fd);
	if (exclop <= 0) {
		ret = 0;
		goto out;
	}

	if (!enqueue) {
		error(
	"unable to start %s, another exclusive operation '%s' in progress",
			get_fs_exclop_name(start),
			get_fs_exclop_name(exclop));
		ret = 1;
		goto out;
	}

	while (exclop > 0) {
		fd_set fds;
		struct timeval tv = { .tv_sec = 60, .tv_usec = 0 };

		FD_ZERO(&fds);
		FD_SET(sysfs_fd, &fds);

		ret = select(sysfs_fd + 1, NULL, NULL, &fds, &tv);
		if (ret < 0) {
			ret = -errno;
			break;
		}
		if (ret > 0) {
			/*
			 * Notified before the timeout, check again before
			 * returning. In case there are more operations
			 * waiting, we want to reduce the chances to race so
			 * reuse the remaining time to randomize the order.
			 */
			tv.tv_sec /= 2;
			ret = select(sysfs_fd + 1, NULL, NULL, &fds, &tv);
			exclop = get_fs_exclop(fd);
			if (exclop <= 0)
				ret = 0;
		}
	}
out:
	close(sysfs_fd);

	return ret;
}
