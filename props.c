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
#include <fcntl.h>
#include <unistd.h>

#include "ctree.h"
#include "commands.h"
#include "utils.h"
#include "props.h"

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
		fprintf(stderr, "ERROR: open %s failed. %s\n",
				object, strerror(-ret));
		goto out;
	}

	ret = ioctl(fd, BTRFS_IOC_SUBVOL_GETFLAGS, &flags);
	if (ret < 0) {
		ret = -errno;
		fprintf(stderr, "ERROR: failed to get flags for %s. %s\n",
				object, strerror(-ret));
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
		fprintf(stderr, "ERROR: invalid value for property.\n");
		goto out;
	}

	ret = ioctl(fd, BTRFS_IOC_SUBVOL_SETFLAGS, &flags);
	if (ret < 0) {
		ret = -errno;
		fprintf(stderr, "ERROR: failed to set flags for %s. %s\n",
				object, strerror(-ret));
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

const struct prop_handler prop_handlers[] = {
	{"ro", "Set/get read-only flag of subvolume.", 0, prop_object_subvol,
	 prop_read_only},
	{"label", "Set/get label of device.", 0,
	 prop_object_dev | prop_object_root, prop_label},
	{0, 0, 0, 0, 0}
};
