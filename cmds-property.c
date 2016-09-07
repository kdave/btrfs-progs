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

#include "commands.h"
#include "props.h"
#include "ctree.h"
#include "utils.h"

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

static int get_fsid(const char *path, u8 *fsid, int silent)
{
	int ret;
	int fd;
	struct btrfs_ioctl_fs_info_args args;

	fd = open(path, O_RDONLY);
	if (fd < 0) {
		ret = -errno;
		if (!silent)
			error("failed to open %s: %s", path,
				strerror(-ret));
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
		error("get_fsid for %s failed: %s", object, strerror(-ret));
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
		error("get_fsid for %s failed: %s", tmp, strerror(-ret));
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

	ret = lstat(object, &st);
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
			ret = prop->handler(type, object, prop->name, NULL);
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
				ret = 50;
				goto out;
			}
		}
	}

	ret = 0;

out:
	return ret;
}

static int setget_prop(int types, const char *object,
		       const char *name, const char *value)
{
	int ret;
	const struct prop_handler *prop = NULL;

	ret = parse_prop(name, prop_handlers, &prop);
	if (ret == -1) {
		error("unknown property: %s", name);
		ret = 40;
		goto out;
	}

	types &= prop->types;
	if (!types) {
		error("object is not compatible with property: %s", prop->name);
		ret = 47;
		goto out;
	}

	if (count_bits(types) > 1) {
		error("type of object is ambiguous, please use option -t");
		ret = 48;
		goto out;
	}

	if (value && prop->read_only) {
		error("property is read-only property: %s",
				prop->name);
		ret = 51;
		goto out;
	}

	ret = prop->handler(types, object, name, value);

	if (ret < 0)
		ret = 50;
	else
		ret = 0;

out:
	return ret;

}

static void parse_args(int argc, char **argv,
		       const char * const *usage_str,
		       int *types, char **object,
		       char **name, char **value, int min_nonopt_args)
{
	int ret;
	char *type_str = NULL;
	int max_nonopt_args = 1;

	optind = 1;
	while (1) {
		int c = getopt(argc, argv, "t:");
		if (c < 0)
			break;

		switch (c) {
		case 't':
			type_str = optarg;
			break;
		default:
			usage(usage_str);
		}
	}

	if (name)
		max_nonopt_args++;
	if (value)
		max_nonopt_args++;

	if (check_argc_min(argc - optind, min_nonopt_args) ||
	    check_argc_max(argc - optind, max_nonopt_args))
		usage(usage_str);

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
			usage(usage_str);
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
			error("failed to detect object type: %s",
				strerror(-ret));
			usage(usage_str);
		}
		if (!*types) {
			error("object is not a btrfs object: %s", *object);
			usage(usage_str);
		}
	}
}

static const char * const cmd_property_get_usage[] = {
	"btrfs property get [-t <type>] <object> [<name>]",
	"Gets a property from a btrfs object.",
	"If no name is specified, all properties for the given object are",
	"printed.",
	"A filesystem object can be a the filesystem itself, a subvolume,",
	"an inode or a device. The '-t <type>' option can be used to explicitly",
	"specify what type of object you meant. This is only needed when a",
	"property could be set for more then one object type. Possible types",
	"are s[ubvol], f[ilesystem], i[node] and d[evice].",
	NULL
};

static int cmd_property_get(int argc, char **argv)
{
	int ret;
	char *object = NULL;
	char *name = NULL;
	int types = 0;

	parse_args(argc, argv, cmd_property_get_usage, &types, &object, &name,
		   NULL, 1);

	if (name)
		ret = setget_prop(types, object, name, NULL);
	else
		ret = dump_props(types, object, 0);

	return ret;
}

static const char * const cmd_property_set_usage[] = {
	"btrfs property set [-t <type>] <object> <name> <value>",
	"Sets a property on a btrfs object.",
	"Please see the help of 'btrfs property get' for a description of",
	"objects and object types.",
	NULL
};

static int cmd_property_set(int argc, char **argv)
{
	int ret;
	char *object = NULL;
	char *name = NULL;
	char *value = NULL;
	int types = 0;

	parse_args(argc, argv, cmd_property_set_usage, &types,
		   &object, &name, &value, 3);

	ret = setget_prop(types, object, name, value);

	return ret;
}

static const char * const cmd_property_list_usage[] = {
	"btrfs property list [-t <type>] <object>",
	"Lists available properties with their descriptions for the given object.",
	"Please see the help of 'btrfs property get' for a description of",
	"objects and object types.",
	NULL
};

static int cmd_property_list(int argc, char **argv)
{
	int ret;
	char *object = NULL;
	int types = 0;

	parse_args(argc, argv, cmd_property_list_usage,
		   &types, &object, NULL, NULL, 1);

	ret = dump_props(types, object, 1);

	return ret;
}

static const char property_cmd_group_info[] =
"modify properties of filesystem objects";

const struct cmd_group property_cmd_group = {
	property_cmd_group_usage, property_cmd_group_info, {
		{ "get", cmd_property_get,
			cmd_property_get_usage, NULL, 0 },
		{ "set", cmd_property_set,
			cmd_property_set_usage, NULL, 0 },
		{ "list", cmd_property_list,
			cmd_property_list_usage, NULL, 0 },
		NULL_CMD_STRUCT
	}
};

int cmd_property(int argc, char **argv)
{
	return handle_command_group(&property_cmd_group, argc, argv);
}
