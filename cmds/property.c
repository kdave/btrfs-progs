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

#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <unistd.h>
#include <fcntl.h>
#include <sys/ioctl.h>
#include <sys/stat.h>
#include <sys/xattr.h>
#include <uuid/uuid.h>
#include <btrfsutil.h>
#include "cmds/commands.h"
#include "cmds/props.h"
#include "kernel-shared/ctree.h"
#include "common/open-utils.h"
#include "common/utils.h"
#include "common/help.h"

#define XATTR_BTRFS_PREFIX     "btrfs."
#define XATTR_BTRFS_PREFIX_LEN (sizeof(XATTR_BTRFS_PREFIX) - 1)

/*
 * Defined as synonyms in attr/xattr.h
 */
#ifndef ENOATTR
#define ENOATTR ENODATA
#endif

static int subvolume_clear_received_uuid(const char *path)
{
	struct btrfs_ioctl_received_subvol_args args = {};
	int ret;
	int fd;

	fd = open(path, O_RDONLY | O_NOATIME);
	if (fd == -1)
		return -errno;

	ret = ioctl(fd, BTRFS_IOC_SET_RECEIVED_SUBVOL, &args);
	if (ret == -1) {
		close(fd);
		return -errno;
	}
	close(fd);

	return 0;
}

static int prop_read_only(enum prop_object_type type,
			  const char *object,
			  const char *name,
			  const char *value,
			  bool force)
{
	enum btrfs_util_error err;
	bool read_only;

	if (value) {
		struct btrfs_util_subvolume_info info = {};
		bool is_ro = false;
		bool do_clear_received_uuid = false;

		if (!strcmp(value, "true")) {
			read_only = true;
		} else if (!strcmp(value, "false")) {
			read_only = false;
		} else {
			error("invalid value for property: %s", value);
			return -EINVAL;
		}
		err = btrfs_util_get_subvolume_read_only(object, &is_ro);
		if (err) {
			error_btrfs_util(err);
			return -errno;
		}
		/* No change if already read-only */
		if (is_ro && read_only)
			return 0;

		err = btrfs_util_subvolume_info(object, 0, &info);
		if (err)
			warning("cannot read subvolume info");
		if (is_ro && !uuid_is_null(info.received_uuid)) {
			pr_verbose(2, "ro->rw switch but has set receive_uuid");

			if (force) {
				do_clear_received_uuid = true;
			} else {
				error(
"cannot flip ro->rw with received_uuid set, use force if you really want that");
				return -EPERM;
			}
		}
		if (!is_ro && !uuid_is_null(info.received_uuid))
			warning("read-write subvolume with received_uuid, this is bad");

		err = btrfs_util_set_subvolume_read_only(object, read_only);
		if (err) {
			error_btrfs_util(err);
			return -errno;
		}
		if (do_clear_received_uuid) {
			int ret;
			char uuid_str[BTRFS_UUID_UNPARSED_SIZE];

			uuid_unparse(info.received_uuid, uuid_str);
			pr_verbose(2, "force used, clearing received_uuid, previously %s",
					uuid_str);
			ret = subvolume_clear_received_uuid(object);
			if (ret < 0)
				warning("failed to clear received_uuid: %m");
		}
	} else {
		err = btrfs_util_get_subvolume_read_only(object, &read_only);
		if (err) {
			error_btrfs_util(err);
			return -errno;
		}

		printf("ro=%s\n", read_only ? "true" : "false");
	}

	return 0;
}

static int prop_label(enum prop_object_type type,
		      const char *object,
		      const char *name,
		      const char *value,
		      bool force)
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
			    const char *value,
			    bool force)
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
		error("failed to open %s: %m", object);
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

	if (value) {
		sret = fsetxattr(fd, xattr_name, value, strlen(value), 0);
	} else {
		sret = fgetxattr(fd, xattr_name, NULL, 0);
	}
	if (sret < 0) {
		ret = -errno;
		if (ret != -ENOATTR)
			error("failed to %s compression for %s: %m",
			      value ? "set" : "get", object);
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
			error("failed to get compression for %s: %m", object);
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
	{
		.name ="ro",
		.desc = "read-only status of a subvolume",
		.read_only = 0,
		.types = prop_object_subvol,
	 	.handler = prop_read_only
	},
	{
		.name = "label",
		.desc = "label of the filesystem",
		.read_only = 0,
		.types = prop_object_dev | prop_object_root,
		.handler = prop_label
	},
	{
		.name = "compression",
		.desc = "compression algorithm for the file or directory",
		.read_only = 0,
		.types = prop_object_inode,
		.handler = prop_compression
	},
	{NULL, NULL, 0, 0, NULL}
};

static const char * const property_cmd_group_usage[] = {
	"btrfs property get/set/list [-t <type>] <object> [<name>] [value]",
	NULL
};

static int parse_prop(const char *arg, const struct prop_handler *props,
		      const struct prop_handler **prop_ret)
{
	const struct prop_handler *prop = props;

	for (; prop->name; prop++) {
		if (!strcmp(prop->name, arg)) {
			*prop_ret = prop;
			return 0;
		}
	}

	return -1;
}

static int check_btrfs_object(const char *object)
{
	int ret;
	u8 fsid[BTRFS_FSID_SIZE];

	ret = get_fsid(object, fsid, 0);
	if (ret < 0)
		ret = 0;
	else
		ret = 1;
	return ret;
}

static int check_is_root(const char *object)
{
	int ret;
	u8 fsid[BTRFS_FSID_SIZE];
	u8 fsid2[BTRFS_FSID_SIZE];
	char *tmp = NULL;
	char *rp;

	rp = realpath(object, NULL);
	if (!rp) {
		ret = -errno;
		goto out;
	}
	if (!strcmp(rp, "/")) {
		ret = 0;
		goto out;
	}

	tmp = malloc(strlen(object) + 5);
	if (!tmp) {
		ret = -ENOMEM;
		goto out;
	}
	strcpy(tmp, object);
	if (tmp[strlen(tmp) - 1] != '/')
		strcat(tmp, "/");
	strcat(tmp, "..");

	ret = get_fsid(object, fsid, 0);
	if (ret < 0) {
		errno = -ret;
		error("get_fsid for %s failed: %m", object);
		goto out;
	}

	ret = get_fsid(tmp, fsid2, 1);
	if (ret == -ENOTTY) {
		ret = 0;
		goto out;
	} else if (ret == -ENOTDIR) {
		ret = 1;
		goto out;
	} else if (ret < 0) {
		errno = -ret;
		error("get_fsid for %s failed: %m", tmp);
		goto out;
	}

	if (memcmp(fsid, fsid2, BTRFS_FSID_SIZE)) {
		ret = 0;
		goto out;
	}

	ret = 1;

out:
	free(tmp);
	free(rp);
	return ret;
}

static int count_bits(int v)
{
	unsigned int tmp = (unsigned int)v;
	int cnt = 0;

	while (tmp) {
		if (tmp & 1)
			cnt++;
		tmp >>= 1;
	}
	return cnt;
}

static int autodetect_object_types(const char *object, int *types_out)
{
	int ret;
	int is_btrfs_object;
	int types = 0;
	struct stat st;

	is_btrfs_object = check_btrfs_object(object);

	ret = stat(object, &st);
	if (ret < 0) {
		ret = -errno;
		goto out;
	}

	if (is_btrfs_object) {
		types |= prop_object_inode;
		if (st.st_ino == BTRFS_FIRST_FREE_OBJECTID)
			types |= prop_object_subvol;

		ret = check_is_root(object);
		if (ret < 0)
			goto out;
		if (!ret)
			types |= prop_object_root;
	}

	if (S_ISBLK(st.st_mode))
		types |= prop_object_dev;

	ret = 0;
	*types_out = types;

out:
	return ret;
}

static int dump_prop(const struct prop_handler *prop,
		     const char *object,
		     int types,
		     int type,
		     int name_and_help)
{
	int ret = 0;

	if ((types & type) && (prop->types & type)) {
		if (!name_and_help)
			ret = prop->handler(type, object, prop->name, NULL, false);
		else
			printf("%-20s%s\n", prop->name, prop->desc);
	}
	return ret;
}

static int dump_props(int types, const char *object, int name_and_help)
{
	int ret;
	int i;
	int j;
	const struct prop_handler *prop;

	for (i = 0; prop_handlers[i].name; i++) {
		prop = &prop_handlers[i];
		for (j = 1; j < __prop_object_max; j <<= 1) {
			ret = dump_prop(prop, object, types, j, name_and_help);
			if (ret < 0) {
				ret = 1;
				goto out;
			}
		}
	}

	ret = 0;

out:
	return ret;
}

static int setget_prop(int types, const char *object,
		       const char *name, const char *value, bool force)
{
	int ret;
	const struct prop_handler *prop = NULL;

	ret = parse_prop(name, prop_handlers, &prop);
	if (ret == -1) {
		error("unknown property: %s", name);
		return 1;
	}

	types &= prop->types;
	if (!types) {
		error("object is not compatible with property: %s", prop->name);
		return 1;
	}

	if (count_bits(types) > 1) {
		error("type of object is ambiguous, please use option -t");
		return 1;
	}

	if (value && prop->read_only) {
		error("property is read-only property: %s",
				prop->name);
		return 1;
	}

	ret = prop->handler(types, object, name, value, force);

	if (ret < 0)
		ret = 1;
	else
		ret = 0;

	return ret;

}

static int parse_args(const struct cmd_struct *cmd, int argc, char **argv,
		       int *types, char **object,
		       char **name, char **value, int min_nonopt_args,
		       bool *force)
{
	int ret;
	char *type_str = NULL;
	int max_nonopt_args = 1;

	*force = false;

	optind = 1;
	while (1) {
		int c = getopt(argc, argv, "ft:");
		if (c < 0)
			break;

		switch (c) {
		case 'f':
			/* TODO: do not accept for get/list */
			*force = true;
			break;
		case 't':
			type_str = optarg;
			break;
		default:
			usage_unknown_option(cmd, argv);
		}
	}

	if (name)
		max_nonopt_args++;
	if (value)
		max_nonopt_args++;

	if (check_argc_min(argc - optind, min_nonopt_args) ||
	    check_argc_max(argc - optind, max_nonopt_args))
		return 1;

	*types = 0;
	if (type_str) {
		if (!strcmp(type_str, "s") || !strcmp(type_str, "subvol")) {
			*types = prop_object_subvol;
		} else if (!strcmp(type_str, "f") ||
			   !strcmp(type_str, "filesystem")) {
			*types = prop_object_root;
		} else if (!strcmp(type_str, "i") ||
			   !strcmp(type_str, "inode")) {
			*types = prop_object_inode;
		} else if (!strcmp(type_str, "d") ||
			   !strcmp(type_str, "device")) {
			*types = prop_object_dev;
		} else {
			error("invalid object type: %s", type_str);
			return 1;
		}
	}

	*object = argv[optind++];
	if (optind < argc)
		*name = argv[optind++];
	if (optind < argc)
		*value = argv[optind++];

	if (!*types) {
		ret = autodetect_object_types(*object, types);
		if (ret < 0) {
			errno = -ret;
			error("failed to detect object type: %m");
			return 1;
		}
		if (!*types) {
			error("object is not a btrfs object: %s", *object);
			return 1;
		}
	}

	return 0;
}

static const char * const cmd_property_get_usage[] = {
	"btrfs property get [-t <type>] <object> [<name>]",
	"Get a property value of a btrfs object",
	"Get a property value of a btrfs object. If no name is specified, all",
	"properties for the given object are printed.",
	"A filesystem object can be the filesystem itself, a subvolume,",
	"an inode or a device. The option -t can be used to explicitly",
	"specify what type of object you meant. This is only needed when a",
	"property could be set for more then one object type.",
	"",
	"Possible values for type are: inode, subvol, filesystem, device.",
	"They can be abbreviated to the first letter, i/s/f/d",
	"",
	"-t <TYPE>       list properties for the given object type (inode, subvol,",
	"                filesystem, device)",
	NULL
};

static int cmd_property_get(const struct cmd_struct *cmd,
			    int argc, char **argv)
{
	int ret;
	char *object = NULL;
	char *name = NULL;
	int types = 0;
	bool force;

	if (parse_args(cmd, argc, argv, &types, &object, &name, NULL, 1, &force))
		return 1;

	if (name)
		ret = setget_prop(types, object, name, NULL, force);
	else
		ret = dump_props(types, object, 0);

	return ret;
}
static DEFINE_SIMPLE_COMMAND(property_get, "get");

static const char * const cmd_property_set_usage[] = {
	"btrfs property set [-f] [-t <type>] <object> <name> <value>",
	"Set a property on a btrfs object",
	"Set a property on a btrfs object where object is a path to file or",
	"directory and can also represent the filesystem or device based on the type",
	"",
	"-t <TYPE>       list properties for the given object type (inode, subvol,",
	"                filesystem, device)",
	"-f              force the change, could potentially break something\n",
	NULL
};

static int cmd_property_set(const struct cmd_struct *cmd,
			    int argc, char **argv)
{
	int ret;
	char *object = NULL;
	char *name = NULL;
	char *value = NULL;
	int types = 0;
	bool force = false;

	if (parse_args(cmd, argc, argv, &types, &object, &name, &value, 3, &force))
		return 1;

	ret = setget_prop(types, object, name, value, force);

	return ret;
}
static DEFINE_SIMPLE_COMMAND(property_set, "set");

static const char * const cmd_property_list_usage[] = {
	"btrfs property list [-t <type>] <object>",
	"Lists available properties with their descriptions for the given object",
	"Lists available properties with their descriptions for the given object",
	"See the help of 'btrfs property get' for a description of",
	"objects and object types.",
	"",
	"-t <TYPE>       list properties for the given object type (inode, subvol,",
	"                filesystem, device)",
	NULL
};

static int cmd_property_list(const struct cmd_struct *cmd,
			     int argc, char **argv)
{
	int ret;
	char *object = NULL;
	int types = 0;
	bool force;

	if (parse_args(cmd, argc, argv, &types, &object, NULL, NULL, 1, &force))
		return 1;

	ret = dump_props(types, object, 1);

	return ret;
}
static DEFINE_SIMPLE_COMMAND(property_list, "list");

static const char property_cmd_group_info[] =
"modify properties of filesystem objects";

static const struct cmd_group property_cmd_group = {
	property_cmd_group_usage, property_cmd_group_info, {
		&cmd_struct_property_get,
		&cmd_struct_property_set,
		&cmd_struct_property_list,
		NULL
	}
};

DEFINE_GROUP_COMMAND_TOKEN(property);
