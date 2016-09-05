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
#include <sys/types.h>
#include <sys/xattr.h>
#include <fcntl.h>
#include <unistd.h>

#include "ctree.h"
#include "commands.h"
#include "utils.h"
#include "props.h"

#define XATTR_BTRFS_PREFIX     "btrfs."
#define XATTR_BTRFS_PREFIX_LEN (sizeof(XATTR_BTRFS_PREFIX) - 1)

/*
 * Defined as synonyms in attr/xattr.h
 */
#ifndef ENOATTR
#define ENOATTR ENODATA
#endif

static int prop_read_only(enum prop_object_type type,
			  const char *object,
			  const char *name,
			  const char *value)
{
	int ret = 0;
	int fd = -1;
	u64 flags = 0;

	fd = open(object, O_RDONLY);
	if (fd < 0) {
		ret = -errno;
		error("failed to open %s: %s", object, strerror(-ret));
		goto out;
	}

	ret = ioctl(fd, BTRFS_IOC_SUBVOL_GETFLAGS, &flags);
	if (ret < 0) {
		ret = -errno;
		error("failed to get flags for %s: %s", object,
				strerror(-ret));
		goto out;
	}

	if (!value) {
		if (flags & BTRFS_SUBVOL_RDONLY)
			fprintf(stdout, "ro=true\n");
		else
			fprintf(stdout, "ro=false\n");
		ret = 0;
		goto out;
	}

	if (!strcmp(value, "true")) {
		flags |= BTRFS_SUBVOL_RDONLY;
	} else if (!strcmp(value, "false")) {
		flags = flags & ~BTRFS_SUBVOL_RDONLY;
	} else {
		ret = -EINVAL;
		error("invalid value for property: %s", value);
		goto out;
	}

	ret = ioctl(fd, BTRFS_IOC_SUBVOL_SETFLAGS, &flags);
	if (ret < 0) {
		ret = -errno;
		error("failed to set flags for %s: %s", object,
				strerror(-ret));
		goto out;
	}

out:
	if (fd != -1)
		close(fd);
	return ret;
}

static int prop_label(enum prop_object_type type,
		      const char *object,
		      const char *name,
		      const char *value)
{
	int ret;

	if (value) {
		ret = set_label((char *) object, (char *) value);
	} else {
		char label[BTRFS_LABEL_SIZE];

		ret = get_label((char *) object, label);
		if (!ret)
			fprintf(stdout, "label=%s\n", label);
	}

	return ret;
}

static int prop_compression(enum prop_object_type type,
			    const char *object,
			    const char *name,
			    const char *value)
{
	int ret;
	ssize_t sret;
	int fd = -1;
	DIR *dirstream = NULL;
	char *buf = NULL;
	char *xattr_name = NULL;
	int open_flags = value ? O_RDWR : O_RDONLY;

	fd = open_file_or_dir3(object, &dirstream, open_flags);
	if (fd == -1) {
		ret = -errno;
		error("failed to open %s: %s", object, strerror(-ret));
		goto out;
	}

	xattr_name = malloc(XATTR_BTRFS_PREFIX_LEN + strlen(name) + 1);
	if (!xattr_name) {
		ret = -ENOMEM;
		goto out;
	}
	memcpy(xattr_name, XATTR_BTRFS_PREFIX, XATTR_BTRFS_PREFIX_LEN);
	memcpy(xattr_name + XATTR_BTRFS_PREFIX_LEN, name, strlen(name));
	xattr_name[XATTR_BTRFS_PREFIX_LEN + strlen(name)] = '\0';

	if (value)
		sret = fsetxattr(fd, xattr_name, value, strlen(value), 0);
	else
		sret = fgetxattr(fd, xattr_name, NULL, 0);
	if (sret < 0) {
		ret = -errno;
		if (ret != -ENOATTR)
			error("failed to %s compression for %s: %s",
			      value ? "set" : "get", object, strerror(-ret));
		else
			ret = 0;
		goto out;
	}
	if (!value) {
		size_t len = sret;

		buf = malloc(len);
		if (!buf) {
			ret = -ENOMEM;
			goto out;
		}
		sret = fgetxattr(fd, xattr_name, buf, len);
		if (sret < 0) {
			ret = -errno;
			error("failed to get compression for %s: %s",
			      object, strerror(-ret));
			goto out;
		}
		fprintf(stdout, "compression=%.*s\n", (int)len, buf);
	}

	ret = 0;
out:
	free(xattr_name);
	free(buf);
	if (fd >= 0)
		close_file_or_dir(fd, dirstream);

	return ret;
}

const struct prop_handler prop_handlers[] = {
	{"ro", "Set/get read-only flag of subvolume.", 0, prop_object_subvol,
	 prop_read_only},
	{"label", "Set/get label of device.", 0,
	 prop_object_dev | prop_object_root, prop_label},
	{"compression", "Set/get compression for a file or directory", 0,
	 prop_object_inode, prop_compression},
	{NULL, NULL, 0, 0, NULL}
};
