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

#include <sys/stat.h>
#include <sys/sysmacros.h>
#include <sys/ioctl.h>
#include <linux/major.h>
#include <linux/kdev_t.h>
#include <linux/loop.h>
#include <linux/limits.h>
#include <fcntl.h>
#include <unistd.h>
#include <stdlib.h>
#include <stdio.h>
#include <mntent.h>
#include <string.h>
#include <errno.h>
#include <ctype.h>
#include "common/path-utils.h"

/*
 * Check if @path is a block device node
 * Returns:
 * 1  - path is a block device
 * 0  - not a block device
 * <0 - negative errno
 */
int path_is_block_device(const char *path)
{
	struct stat statbuf;

	if (stat(path, &statbuf) < 0)
		return -errno;

	return !!S_ISBLK(statbuf.st_mode);
}

/*
 * check if given path is a mount point
 * return 1 if yes. 0 if no. -1 for error
 */
int path_is_mount_point(const char *path)
{
	FILE *f;
	struct mntent *mnt;
	int ret = 0;

	f = setmntent("/proc/self/mounts", "r");
	if (f == NULL)
		return -1;

	while ((mnt = getmntent(f)) != NULL) {
		if (strcmp(mnt->mnt_dir, path))
			continue;
		ret = 1;
		break;
	}
	endmntent(f);
	return ret;
}

int path_is_reg_file(const char *path)
{
	struct stat statbuf;

	if (stat(path, &statbuf) < 0)
		return -errno;
	return S_ISREG(statbuf.st_mode);
}

int path_exists(const char *path)
{
	struct stat statbuf;
	int ret;

	ret = stat(path, &statbuf);
	if (ret < 0) {
		if (errno == ENOENT)
			return 0;
		else
			return -errno;
	}
	return 1;
}

/* checks if a device is a loop device */
static int is_loop_device(const char *device)
{
	struct stat statbuf;

	if(stat(device, &statbuf) < 0)
		return -errno;

	return (S_ISBLK(statbuf.st_mode) &&
		MAJOR(statbuf.st_rdev) == LOOP_MAJOR);
}

/*
 * Takes a loop device path (e.g. /dev/loop0) and returns
 * the associated file (e.g. /images/my_btrfs.img) using
 * loopdev API
 */
static int resolve_loop_device_with_loopdev(const char* loop_dev, char* loop_file)
{
	int fd;
	int ret;
	struct loop_info64 lo64;

	fd = open(loop_dev, O_RDONLY | O_NONBLOCK);
	if (fd < 0)
		return -errno;
	ret = ioctl(fd, LOOP_GET_STATUS64, &lo64);
	if (ret < 0) {
		ret = -errno;
		goto out;
	}

	memcpy(loop_file, lo64.lo_file_name, sizeof(lo64.lo_file_name));
	loop_file[sizeof(lo64.lo_file_name)] = 0;

out:
	close(fd);

	return ret;
}

/*
 * Takes a loop device path (e.g. /dev/loop0) and returns
 * the associated file (e.g. /images/my_btrfs.img)
 */
static int resolve_loop_device(const char* loop_dev, char* loop_file,
		int max_len)
{
	int ret;
	FILE *f;
	char fmt[20];
	char p[PATH_MAX];
	char real_loop_dev[PATH_MAX];

	if (!realpath(loop_dev, real_loop_dev))
		return -errno;
	snprintf(p, PATH_MAX, "/sys/block/%s/loop/backing_file", strrchr(real_loop_dev, '/'));
	if (!(f = fopen(p, "r"))) {
		if (errno == ENOENT)
			/*
			 * It's possibly a partitioned loop device, which is
			 * resolvable with loopdev API.
			 */
			return resolve_loop_device_with_loopdev(loop_dev, loop_file);
		return -errno;
	}

	snprintf(fmt, 20, "%%%i[^\n]", max_len - 1);
	ret = fscanf(f, fmt, loop_file);
	fclose(f);
	if (ret == EOF)
		return -errno;

	return 0;
}

/*
 * Checks whether a and b are identical or device
 * files associated with the same block device
 */
static int is_same_blk_file(const char* a, const char* b)
{
	struct stat st_buf_a, st_buf_b;
	char real_a[PATH_MAX];
	char real_b[PATH_MAX];

	if (!realpath(a, real_a))
		strncpy_null(real_a, a);

	if (!realpath(b, real_b))
		strncpy_null(real_b, b);

	/* Identical path? */
	if (strcmp(real_a, real_b) == 0)
		return 1;

	if (stat(a, &st_buf_a) < 0 || stat(b, &st_buf_b) < 0) {
		if (errno == ENOENT)
			return 0;
		return -errno;
	}

	/* Same blockdevice? */
	if (S_ISBLK(st_buf_a.st_mode) && S_ISBLK(st_buf_b.st_mode) &&
	    st_buf_a.st_rdev == st_buf_b.st_rdev) {
		return 1;
	}

	/* Hardlink? */
	if (st_buf_a.st_dev == st_buf_b.st_dev &&
	    st_buf_a.st_ino == st_buf_b.st_ino) {
		return 1;
	}

	return 0;
}

/*
 * Checks if a and b are identical or device files associated with the same
 * block device or if one file is a loop device that uses the other file.
 */
int is_same_loop_file(const char *a, const char *b)
{
	char res_a[PATH_MAX];
	char res_b[PATH_MAX];
	const char* final_a = NULL;
	const char* final_b = NULL;
	int ret;

	/* Resolve a if it is a loop device */
	if ((ret = is_loop_device(a)) < 0) {
		if (ret == -ENOENT)
			return 0;
		return ret;
	} else if (ret) {
		ret = resolve_loop_device(a, res_a, sizeof(res_a));
		if (ret < 0) {
			if (errno != EPERM)
				return ret;
		} else {
			final_a = res_a;
		}
	} else {
		final_a = a;
	}

	/* Resolve b if it is a loop device */
	if ((ret = is_loop_device(b)) < 0) {
		if (ret == -ENOENT)
			return 0;
		return ret;
	} else if (ret) {
		ret = resolve_loop_device(b, res_b, sizeof(res_b));
		if (ret < 0) {
			if (errno != EPERM)
				return ret;
		} else {
			final_b = res_b;
		}
	} else {
		final_b = b;
	}

	return is_same_blk_file(final_a, final_b);
}

/* Checks if a file exists and is a block or regular file*/
int path_is_reg_or_block_device(const char *filename)
{
	struct stat st_buf;

	if (stat(filename, &st_buf) < 0) {
		if(errno == ENOENT)
			return 0;
		else
			return -errno;
	}

	return (S_ISBLK(st_buf.st_mode) || S_ISREG(st_buf.st_mode));
}

/*
 * Resolve a pathname to a device mapper node to /dev/mapper/<name>
 * Returns NULL on invalid input or malloc failure; Other failures
 * will be handled by the caller using the input pathname.
 */
char *canonicalize_dm_name(const char *ptname)
{
	FILE *f;
	size_t sz;
	char path[PATH_MAX], name[PATH_MAX], *res = NULL;

	if (!ptname || !*ptname)
		return NULL;

	snprintf(path, sizeof(path), "/sys/block/%s/dm/name", ptname);
	if (!(f = fopen(path, "r")))
		return NULL;

	/* read <name>\n from sysfs */
	if (fgets(name, sizeof(name), f) && (sz = strlen(name)) > 1) {
		name[sz - 1] = '\0';
		snprintf(path, sizeof(path), "/dev/mapper/%s", name);

		if (access(path, F_OK) == 0)
			res = strdup(path);
	}
	fclose(f);
	return res;
}

/*
 * Resolve a pathname to a canonical device node, e.g. /dev/sda1 or
 * to a device mapper pathname.
 * Returns NULL on invalid input or malloc failure; Other failures
 * will be handled by the caller using the input pathname.
 */
char *canonicalize_path(const char *path)
{
	char *canonical, *p;

	if (!path || !*path)
		return NULL;

	canonical = realpath(path, NULL);
	if (!canonical)
		return strdup(path);
	p = strrchr(canonical, '/');
	if (p && strncmp(p, "/dm-", 4) == 0 && isdigit(*(p + 4))) {
		char *dm = canonicalize_dm_name(p + 1);

		if (dm) {
			free(canonical);
			return dm;
		}
	}
	return canonical;
}

/*
 * __strncpy_null - strncpy with null termination
 * @dest:	the target array
 * @src:	the source string
 * @n:		maximum bytes to copy (size of *dest)
 *
 * Like strncpy, but ensures destination is null-terminated.
 *
 * Copies the string pointed to by src, including the terminating null
 * byte ('\0'), to the buffer pointed to by dest, up to a maximum
 * of n bytes.  Then ensure that dest is null-terminated.
 */
char *__strncpy_null(char *dest, const char *src, size_t n)
{
	strncpy(dest, src, n);
	if (n > 0)
		dest[n - 1] = '\0';
	return dest;
}

/*
 * Test if path is a directory
 * Returns:
 *   0 - path exists but it is not a directory
 *   1 - path exists and it is a directory
 * < 0 - error
 */
int path_is_dir(const char *path)
{
	struct stat st;
	int ret;

	ret = stat(path, &st);
	if (ret < 0)
		return -errno;

	return !!S_ISDIR(st.st_mode);
}

/*
 * Copy a path argument from SRC to DEST and check the SRC length if it's at
 * most PATH_MAX and fits into DEST. DESTLEN is supposed to be exact size of
 * the buffer.
 * The destination buffer is zero terminated.
 * Return < 0 for error, 0 otherwise.
 */
int arg_copy_path(char *dest, const char *src, int destlen)
{
	size_t len = strlen(src);

	if (len >= PATH_MAX || len >= destlen)
		return -ENAMETOOLONG;

	__strncpy_null(dest, src, destlen);

	return 0;
}

int path_cat_out(char *out, const char *p1, const char *p2)
{
	int p1_len = strlen(p1);
	int p2_len = strlen(p2);

	if (p1_len + p2_len + 2 >= PATH_MAX)
		return -ENAMETOOLONG;

	if (p1_len && p1[p1_len - 1] == '/')
		p1_len--;
	if (p2_len && p2[p2_len - 1] == '/')
		p2_len--;
	sprintf(out, "%.*s/%.*s", p1_len, p1, p2_len, p2);

	return 0;
}

int path_cat3_out(char *out, const char *p1, const char *p2, const char *p3)
{
	int p1_len = strlen(p1);
	int p2_len = strlen(p2);
	int p3_len = strlen(p3);

	if (p1_len + p2_len + p3_len + 3 >= PATH_MAX)
		return -ENAMETOOLONG;

	if (p1_len && p1[p1_len - 1] == '/')
		p1_len--;
	if (p2_len && p2[p2_len - 1] == '/')
		p2_len--;
	if (p3_len && p3[p3_len - 1] == '/')
		p3_len--;
	sprintf(out, "%.*s/%.*s/%.*s", p1_len, p1, p2_len, p2, p3_len, p3);

	return 0;
}

/* Subvolume helper functions */
/*
 * test if name is a correct subvolume name
 * this function return
 * 0-> name is not a correct subvolume name
 * 1-> name is a correct subvolume name
 */
int test_issubvolname(const char *name)
{
	return name[0] != '\0' && !strchr(name, '/') &&
		strcmp(name, ".") && strcmp(name, "..");
}

