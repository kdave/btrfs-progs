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
#include <linux/loop.h>
#include <linux/major.h>
#include <linux/kdev_t.h>
#include <limits.h>
#include <blkid/blkid.h>
#include <sys/vfs.h>
#include <sys/statfs.h>
#include <linux/magic.h>
#include <getopt.h>

#include <btrfsutil.h>

#include "kerncompat.h"
#include "kernel-lib/radix-tree.h"
#include "ctree.h"
#include "disk-io.h"
#include "transaction.h"
#include "crypto/crc32c.h"
#include "common/utils.h"
#include "common/path-utils.h"
#include "common/device-scan.h"
#include "volumes.h"
#include "ioctl.h"
#include "cmds/commands.h"
#include "mkfs/common.h"

static int rand_seed_initialized = 0;
static unsigned short rand_seed[3];

struct btrfs_config bconf;

int btrfs_make_root_dir(struct btrfs_trans_handle *trans,
			struct btrfs_root *root, u64 objectid)
{
	int ret;
	struct btrfs_inode_item inode_item;
	time_t now = time(NULL);

	memset(&inode_item, 0, sizeof(inode_item));
	btrfs_set_stack_inode_generation(&inode_item, trans->transid);
	btrfs_set_stack_inode_size(&inode_item, 0);
	btrfs_set_stack_inode_nlink(&inode_item, 1);
	btrfs_set_stack_inode_nbytes(&inode_item, root->fs_info->nodesize);
	btrfs_set_stack_inode_mode(&inode_item, S_IFDIR | 0755);
	btrfs_set_stack_timespec_sec(&inode_item.atime, now);
	btrfs_set_stack_timespec_nsec(&inode_item.atime, 0);
	btrfs_set_stack_timespec_sec(&inode_item.ctime, now);
	btrfs_set_stack_timespec_nsec(&inode_item.ctime, 0);
	btrfs_set_stack_timespec_sec(&inode_item.mtime, now);
	btrfs_set_stack_timespec_nsec(&inode_item.mtime, 0);
	btrfs_set_stack_timespec_sec(&inode_item.otime, now);
	btrfs_set_stack_timespec_nsec(&inode_item.otime, 0);

	if (root->fs_info->tree_root == root)
		btrfs_set_super_root_dir(root->fs_info->super_copy, objectid);

	ret = btrfs_insert_inode(trans, root, objectid, &inode_item);
	if (ret)
		goto error;

	ret = btrfs_insert_inode_ref(trans, root, "..", 2, objectid, objectid, 0);
	if (ret)
		goto error;

	btrfs_set_root_dirid(&root->root_item, objectid);
	ret = 0;
error:
	return ret;
}

/*
 * Find the mount point for a mounted device.
 * On success, returns 0 with mountpoint in *mp.
 * On failure, returns -errno (not mounted yields -EINVAL)
 * Is noisy on failures, expects to be given a mounted device.
 */
int get_btrfs_mount(const char *dev, char *mp, size_t mp_size)
{
	int ret;
	int fd = -1;

	ret = path_is_block_device(dev);
	if (ret <= 0) {
		if (!ret) {
			error("not a block device: %s", dev);
			ret = -EINVAL;
		} else {
			errno = -ret;
			error("cannot check %s: %m", dev);
		}
		goto out;
	}

	fd = open(dev, O_RDONLY);
	if (fd < 0) {
		ret = -errno;
		error("cannot open %s: %m", dev);
		goto out;
	}

	ret = check_mounted_where(fd, dev, mp, mp_size, NULL, SBREAD_DEFAULT);
	if (!ret) {
		ret = -EINVAL;
	} else { /* mounted, all good */
		ret = 0;
	}
out:
	if (fd != -1)
		close(fd);
	return ret;
}

/*
 * Given a pathname, return a filehandle to:
 * 	the original pathname or,
 * 	if the pathname is a mounted btrfs device, to its mountpoint.
 *
 * On error, return -1, errno should be set.
 */
int open_path_or_dev_mnt(const char *path, DIR **dirstream, int verbose)
{
	char mp[PATH_MAX];
	int ret;

	if (path_is_block_device(path)) {
		ret = get_btrfs_mount(path, mp, sizeof(mp));
		if (ret < 0) {
			/* not a mounted btrfs dev */
			error_on(verbose, "'%s' is not a mounted btrfs device",
				 path);
			errno = EINVAL;
			return -1;
		}
		ret = open_file_or_dir(mp, dirstream);
		error_on(verbose && ret < 0, "can't access '%s': %m",
			 path);
	} else {
		ret = btrfs_open_dir(path, dirstream, 1);
	}

	return ret;
}

/*
 * Do the following checks before calling open_file_or_dir():
 * 1: path is in a btrfs filesystem
 * 2: path is a directory if dir_only is 1
 */
int btrfs_open(const char *path, DIR **dirstream, int verbose, int dir_only)
{
	struct statfs stfs;
	struct stat st;
	int ret;

	if (statfs(path, &stfs) != 0) {
		error_on(verbose, "cannot access '%s': %m", path);
		return -1;
	}

	if (stfs.f_type != BTRFS_SUPER_MAGIC) {
		error_on(verbose, "not a btrfs filesystem: %s", path);
		return -2;
	}

	if (stat(path, &st) != 0) {
		error_on(verbose, "cannot access '%s': %m", path);
		return -1;
	}

	if (dir_only && !S_ISDIR(st.st_mode)) {
		error_on(verbose, "not a directory: %s", path);
		return -3;
	}

	ret = open_file_or_dir(path, dirstream);
	if (ret < 0) {
		error_on(verbose, "cannot access '%s': %m", path);
	}

	return ret;
}

int btrfs_open_dir(const char *path, DIR **dirstream, int verbose)
{
	return btrfs_open(path, dirstream, verbose, 1);
}

int btrfs_open_file_or_dir(const char *path, DIR **dirstream, int verbose)
{
	return btrfs_open(path, dirstream, verbose, 0);
}

/* Checks if a file is used (directly or indirectly via a loop device)
 * by a device in fs_devices
 */
static int blk_file_in_dev_list(struct btrfs_fs_devices* fs_devices,
		const char* file)
{
	int ret;
	struct btrfs_device *device;

	list_for_each_entry(device, &fs_devices->devices, dev_list) {
		if((ret = is_same_loop_file(device->name, file)))
			return ret;
	}

	return 0;
}

/*
 * returns 1 if the device was mounted, < 0 on error or 0 if everything
 * is safe to continue.
 */
int check_mounted(const char* file)
{
	int fd;
	int ret;

	fd = open(file, O_RDONLY);
	if (fd < 0) {
		error("mount check: cannot open %s: %m", file);
		return -errno;
	}

	ret =  check_mounted_where(fd, file, NULL, 0, NULL, SBREAD_DEFAULT);
	close(fd);

	return ret;
}

int check_mounted_where(int fd, const char *file, char *where, int size,
			struct btrfs_fs_devices **fs_dev_ret, unsigned sbflags)
{
	int ret;
	u64 total_devs = 1;
	int is_btrfs;
	struct btrfs_fs_devices *fs_devices_mnt = NULL;
	FILE *f;
	struct mntent *mnt;

	/* scan the initial device */
	ret = btrfs_scan_one_device(fd, file, &fs_devices_mnt,
		    &total_devs, BTRFS_SUPER_INFO_OFFSET, sbflags);
	is_btrfs = (ret >= 0);

	/* scan other devices */
	if (is_btrfs && total_devs > 1) {
		ret = btrfs_scan_devices(0);
		if (ret)
			return ret;
	}

	/* iterate over the list of currently mounted filesystems */
	if ((f = setmntent ("/proc/self/mounts", "r")) == NULL)
		return -errno;

	while ((mnt = getmntent (f)) != NULL) {
		if(is_btrfs) {
			if(strcmp(mnt->mnt_type, "btrfs") != 0)
				continue;

			ret = blk_file_in_dev_list(fs_devices_mnt, mnt->mnt_fsname);
		} else {
			/* ignore entries in the mount table that are not
			   associated with a file*/
			if((ret = path_is_reg_or_block_device(mnt->mnt_fsname)) < 0)
				goto out_mntloop_err;
			else if(!ret)
				continue;

			ret = is_same_loop_file(file, mnt->mnt_fsname);
		}

		if(ret < 0)
			goto out_mntloop_err;
		else if(ret)
			break;
	}

	/* Did we find an entry in mnt table? */
	if (mnt && size && where) {
		strncpy(where, mnt->mnt_dir, size);
		where[size-1] = 0;
	}
	if (fs_dev_ret)
		*fs_dev_ret = fs_devices_mnt;

	ret = (mnt != NULL);

out_mntloop_err:
	endmntent (f);

	return ret;
}

struct pending_dir {
	struct list_head list;
	char name[PATH_MAX];
};

/*
 * Note: this function uses a static per-thread buffer. Do not call this
 * function more than 10 times within one argument list!
 */
const char *pretty_size_mode(u64 size, unsigned mode)
{
	static __thread int ps_index = 0;
	static __thread char ps_array[10][32];
	char *ret;

	ret = ps_array[ps_index];
	ps_index++;
	ps_index %= 10;
	(void)pretty_size_snprintf(size, ret, 32, mode);

	return ret;
}

static const char* unit_suffix_binary[] =
	{ "B", "KiB", "MiB", "GiB", "TiB", "PiB", "EiB"};
static const char* unit_suffix_decimal[] =
	{ "B", "kB", "MB", "GB", "TB", "PB", "EB"};

int pretty_size_snprintf(u64 size, char *str, size_t str_size, unsigned unit_mode)
{
	int num_divs;
	float fraction;
	u64 base = 0;
	int mult = 0;
	const char** suffix = NULL;
	u64 last_size;
	int negative;

	if (str_size == 0)
		return 0;

	negative = !!(unit_mode & UNITS_NEGATIVE);
	unit_mode &= ~UNITS_NEGATIVE;

	if ((unit_mode & ~UNITS_MODE_MASK) == UNITS_RAW) {
		if (negative)
			snprintf(str, str_size, "%lld", size);
		else
			snprintf(str, str_size, "%llu", size);
		return 0;
	}

	if ((unit_mode & ~UNITS_MODE_MASK) == UNITS_BINARY) {
		base = 1024;
		mult = 1024;
		suffix = unit_suffix_binary;
	} else if ((unit_mode & ~UNITS_MODE_MASK) == UNITS_DECIMAL) {
		base = 1000;
		mult = 1000;
		suffix = unit_suffix_decimal;
	}

	/* Unknown mode */
	if (!base) {
		fprintf(stderr, "INTERNAL ERROR: unknown unit base, mode %u\n",
				unit_mode);
		assert(0);
		return -1;
	}

	num_divs = 0;
	last_size = size;
	switch (unit_mode & UNITS_MODE_MASK) {
	case UNITS_TBYTES:
		base *= mult;
		num_divs++;
		/* fallthrough */
	case UNITS_GBYTES:
		base *= mult;
		num_divs++;
		/* fallthrough */
	case UNITS_MBYTES:
		base *= mult;
		num_divs++;
		/* fallthrough */
	case UNITS_KBYTES:
		num_divs++;
		break;
	case UNITS_BYTES:
		base = 1;
		num_divs = 0;
		break;
	default:
		if (negative) {
			s64 ssize = (s64)size;
			s64 last_ssize = ssize;

			while ((ssize < 0 ? -ssize : ssize) >= mult) {
				last_ssize = ssize;
				ssize /= mult;
				num_divs++;
			}
			last_size = (u64)last_ssize;
		} else {
			while (size >= mult) {
				last_size = size;
				size /= mult;
				num_divs++;
			}
		}
		/*
		 * If the value is smaller than base, we didn't do any
		 * division, in that case, base should be 1, not original
		 * base, or the unit will be wrong
		 */
		if (num_divs == 0)
			base = 1;
	}

	if (num_divs >= ARRAY_SIZE(unit_suffix_binary)) {
		str[0] = '\0';
		printf("INTERNAL ERROR: unsupported unit suffix, index %d\n",
				num_divs);
		assert(0);
		return -1;
	}

	if (negative) {
		fraction = (float)(s64)last_size / base;
	} else {
		fraction = (float)last_size / base;
	}

	return snprintf(str, str_size, "%.2f%s", fraction, suffix[num_divs]);
}

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
 * no one except parse_size use it
 */
static int fls64(u64 x)
{
	int i;

	for (i = 0; i <64; i++)
		if (x << i & (1ULL << 63))
			return 64 - i;
	return 64 - i;
}

u64 parse_size(const char *s)
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

int open_file_or_dir3(const char *fname, DIR **dirstream, int open_flags)
{
	int ret;
	struct stat st;
	int fd;

	ret = stat(fname, &st);
	if (ret < 0) {
		return -1;
	}
	if (S_ISDIR(st.st_mode)) {
		*dirstream = opendir(fname);
		if (!*dirstream)
			return -1;
		fd = dirfd(*dirstream);
	} else if (S_ISREG(st.st_mode) || S_ISLNK(st.st_mode)) {
		fd = open(fname, open_flags);
	} else {
		/*
		 * we set this on purpose, in case the caller output
		 * strerror(errno) as success
		 */
		errno = EINVAL;
		return -1;
	}
	if (fd < 0) {
		fd = -1;
		if (*dirstream) {
			closedir(*dirstream);
			*dirstream = NULL;
		}
	}
	return fd;
}

int open_file_or_dir(const char *fname, DIR **dirstream)
{
	return open_file_or_dir3(fname, dirstream, O_RDWR);
}

void close_file_or_dir(int fd, DIR *dirstream)
{
	int old_errno;

	old_errno = errno;
	if (dirstream) {
		closedir(dirstream);
	} else if (fd >= 0) {
		close(fd);
	}

	errno = old_errno;
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

int get_fsid(const char *path, u8 *fsid, int silent)
{
	int ret;
	int fd;
	struct btrfs_ioctl_fs_info_args args;

	fd = open(path, O_RDONLY);
	if (fd < 0) {
		ret = -errno;
		if (!silent)
			error("failed to open %s: %m", path);
		goto out;
	}

	ret = ioctl(fd, BTRFS_IOC_FS_INFO, &args);
	if (ret < 0) {
		ret = -errno;
		goto out;
	}

	memcpy(fsid, args.fsid, BTRFS_FSID_SIZE);
	ret = 0;

out:
	if (fd != -1)
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
		len = strlen(ent->mnt_dir);
		if (strncmp(ent->mnt_dir, path, len) == 0) {
			/* match found and use the latest match */
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

void units_set_mode(unsigned *units, unsigned mode)
{
	unsigned base = *units & UNITS_MODE_MASK;

	*units = base | mode;
}

void units_set_base(unsigned *units, unsigned base)
{
	unsigned mode = *units & ~UNITS_MODE_MASK;

	*units = base | mode;
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

unsigned int get_unit_mode_from_arg(int *argc, char *argv[], int df_mode)
{
	unsigned int unit_mode = UNITS_DEFAULT;
	int arg_i;
	int arg_end;

	for (arg_i = 0; arg_i < *argc; arg_i++) {
		if (!strcmp(argv[arg_i], "--"))
			break;

		if (!strcmp(argv[arg_i], "--raw")) {
			unit_mode = UNITS_RAW;
			argv[arg_i] = NULL;
			continue;
		}
		if (!strcmp(argv[arg_i], "--human-readable")) {
			unit_mode = UNITS_HUMAN_BINARY;
			argv[arg_i] = NULL;
			continue;
		}

		if (!strcmp(argv[arg_i], "--iec")) {
			units_set_mode(&unit_mode, UNITS_BINARY);
			argv[arg_i] = NULL;
			continue;
		}
		if (!strcmp(argv[arg_i], "--si")) {
			units_set_mode(&unit_mode, UNITS_DECIMAL);
			argv[arg_i] = NULL;
			continue;
		}

		if (!strcmp(argv[arg_i], "--kbytes")) {
			units_set_base(&unit_mode, UNITS_KBYTES);
			argv[arg_i] = NULL;
			continue;
		}
		if (!strcmp(argv[arg_i], "--mbytes")) {
			units_set_base(&unit_mode, UNITS_MBYTES);
			argv[arg_i] = NULL;
			continue;
		}
		if (!strcmp(argv[arg_i], "--gbytes")) {
			units_set_base(&unit_mode, UNITS_GBYTES);
			argv[arg_i] = NULL;
			continue;
		}
		if (!strcmp(argv[arg_i], "--tbytes")) {
			units_set_base(&unit_mode, UNITS_TBYTES);
			argv[arg_i] = NULL;
			continue;
		}

		if (!df_mode)
			continue;

		if (!strcmp(argv[arg_i], "-b")) {
			unit_mode = UNITS_RAW;
			argv[arg_i] = NULL;
			continue;
		}
		if (!strcmp(argv[arg_i], "-h")) {
			unit_mode = UNITS_HUMAN_BINARY;
			argv[arg_i] = NULL;
			continue;
		}
		if (!strcmp(argv[arg_i], "-H")) {
			unit_mode = UNITS_HUMAN_DECIMAL;
			argv[arg_i] = NULL;
			continue;
		}
		if (!strcmp(argv[arg_i], "-k")) {
			units_set_base(&unit_mode, UNITS_KBYTES);
			argv[arg_i] = NULL;
			continue;
		}
		if (!strcmp(argv[arg_i], "-m")) {
			units_set_base(&unit_mode, UNITS_MBYTES);
			argv[arg_i] = NULL;
			continue;
		}
		if (!strcmp(argv[arg_i], "-g")) {
			units_set_base(&unit_mode, UNITS_GBYTES);
			argv[arg_i] = NULL;
			continue;
		}
		if (!strcmp(argv[arg_i], "-t")) {
			units_set_base(&unit_mode, UNITS_TBYTES);
			argv[arg_i] = NULL;
			continue;
		}
	}

	for (arg_i = 0, arg_end = 0; arg_i < *argc; arg_i++) {
		if (!argv[arg_i])
			continue;
		argv[arg_end] = argv[arg_i];
		arg_end++;
	}

	*argc = arg_end;

	return unit_mode;
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
