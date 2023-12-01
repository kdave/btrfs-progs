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

#include "kerncompat.h"
#include <unistd.h>
#include <fcntl.h>
#include <uuid/uuid.h>
#include "common/sysfs-utils.h"
#include "common/path-utils.h"
#include "common/utils.h"

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

int sysfs_read_file_u64(const char *name, u64 *value)
{
	int fd = -1;
	int ret;
	char str[32] = { 0 };

	fd = sysfs_open_file(name);
	if (fd < 0)
		return fd;

	ret = sysfs_read_file(fd, str, sizeof(str));
	if (ret < 0)
		goto out;
	/* Raw value in any numeric format should work, followed by a newline. */
	errno = 0;
	*value = strtoull(str, NULL, 0);
out:
	close(fd);
	return ret;
}

int sysfs_read_fsid_file_u64(int fd, const char *name, u64 *value)
{
	int ret;
	char str[32] = { 0 };

	fd = sysfs_open_fsid_file(fd, name);
	if (fd < 0)
		return fd;

	ret = sysfs_read_file(fd, str, sizeof(str));
	if (ret < 0)
		goto out;
	/* Raw value in any numeric format should work, followed by a newline. */
	errno = 0;
	*value = strtoull(str, NULL, 0);
out:
	close(fd);
	return ret;
}
