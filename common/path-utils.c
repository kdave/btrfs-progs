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
/*
 * For dirname() and basename(), but never use basename directly, there's
 * path_basename() with unified GNU behaviour regardless of the includes and
 * conditional defines. See basename(3) for more.
 */
#include <libgen.h>
#include <limits.h>
#include "common/string-utils.h"
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
 * Check if given path is a mount point. (Note: a similar function also exists
 * in libudev so it's been renamed to avoid clash.)
 *
 * Return: 1 if yes,
 *         0 if no,
 *        -1 for error
 */
int path_is_a_mount_point(const char *path)
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

/*
 * Checks whether a and b are identical or device
 * files associated with the same block device
 */
int is_same_blk_file(const char* a, const char* b)
{
	struct stat st_buf_a, st_buf_b;
	char real_a[PATH_MAX];
	char real_b[PATH_MAX];

	if (!realpath(a, real_a))
		strncpy_null(real_a, a, sizeof(real_a));

	if (!realpath(b, real_b))
		strncpy_null(real_b, b, sizeof(real_b));

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
char *path_canonicalize_dm_name(const char *ptname)
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
char *path_canonicalize(const char *path)
{
	char *canonical, *p;

	if (!path || !*path)
		return NULL;

	canonical = realpath(path, NULL);
	if (!canonical)
		return strdup(path);
	p = strrchr(canonical, '/');
	if (p && strncmp(p, "/dm-", 4) == 0 && isdigit(*(p + 4))) {
		char *dm = path_canonicalize_dm_name(p + 1);

		if (dm) {
			free(canonical);
			return dm;
		}
	}
	return canonical;
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
 * Test if a path is recursively contained in parent.  Assumes parent and path
 * are null terminated absolute paths.
 *
 * Returns:
 *   0 - path not contained in parent
 *   1 - path contained in parent
 * < 0 - error
 *
 * e.g. (/, /foo) -> 1
 *      (/foo, /) -> 0
 *      (/foo, /foo/bar/baz) -> 1
 */
int path_is_in_dir(const char *parent, const char *path)
{
	char tmp[PATH_MAX];
	char *curr_dir = tmp;
	int ret;

	strncpy_null(tmp, path, sizeof(tmp));

	while (strcmp(parent, curr_dir) != 0) {
		if (strcmp(curr_dir, "/") == 0) {
			ret = 0;
			goto out;
		}
		curr_dir = dirname(curr_dir);
	}
	ret = 1;

out:
	return ret;
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

	strncpy_null(dest, src, destlen);

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

/*
 * Unified GNU semantics basename helper, never changing the argument. Always
 * use this instead of basename().
 */
char *path_basename(char *path)
{
#if 0
	const char *tmp = strrchr(path, '/');

	/* Special case when the whole path is just "/". */
	if (path[0] == '/' && path[1] == 0)
		return path;

	return tmp ? tmp + 1 : path;
#else
	return basename(path);
#endif
}

/*
 * Return dirname component of path, may change the argument.
 * Own helper for parity with path_basename().
 */
char *path_dirname(char *path)
{
	return dirname(path);
}
