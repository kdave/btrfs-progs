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
#include <sys/stat.h>
#include <sys/vfs.h>
#include <libgen.h>
#include <limits.h>
#include <getopt.h>
#include <uuid/uuid.h>
#include <linux/magic.h>

#include "kerncompat.h"
#include "ioctl.h"
#include "qgroup.h"

#include "ctree.h"
#include "commands.h"
#include "utils.h"
#include "btrfs-list.h"
#include "utils.h"

static int is_subvolume_cleaned(int fd, u64 subvolid)
{
	int ret;
	struct btrfs_ioctl_search_args args;
	struct btrfs_ioctl_search_key *sk = &args.key;

	sk->tree_id = BTRFS_ROOT_TREE_OBJECTID;
	sk->min_objectid = subvolid;
	sk->max_objectid = subvolid;
	sk->min_type = BTRFS_ROOT_ITEM_KEY;
	sk->max_type = BTRFS_ROOT_ITEM_KEY;
	sk->min_offset = 0;
	sk->max_offset = (u64)-1;
	sk->min_transid = 0;
	sk->max_transid = (u64)-1;
	sk->nr_items = 1;

	ret = ioctl(fd, BTRFS_IOC_TREE_SEARCH, &args);
	if (ret < 0)
		return -errno;

	if (sk->nr_items == 0)
		return 1;

	return 0;
}

static int wait_for_subvolume_cleaning(int fd, int count, u64 *ids,
		int sleep_interval)
{
	int ret;
	int i;

	while (1) {
		int clean = 1;

		for (i = 0; i < count; i++) {
			if (!ids[i])
				continue;
			ret = is_subvolume_cleaned(fd, ids[i]);
			if (ret < 0) {
				error(
			    "cannot read status of dead subvolume %llu: %s",
					(unsigned long long)ids[i], strerror(-ret));
				return ret;
			}
			if (ret) {
				printf("Subvolume id %llu is gone\n", ids[i]);
				ids[i] = 0;
			} else {
				clean = 0;
			}
		}
		if (clean)
			break;
		sleep(sleep_interval);
	}

	return 0;
}

static const char * const subvolume_cmd_group_usage[] = {
	"btrfs subvolume <command> <args>",
	NULL
};

static const char * const cmd_subvol_create_usage[] = {
	"btrfs subvolume create [-i <qgroupid>] [<dest>/]<name>",
	"Create a subvolume",
	"Create a subvolume <name> in <dest>.  If <dest> is not given",
	"subvolume <name> will be created in the current directory.",
	"",
	"-i <qgroupid>  add the newly created subvolume to a qgroup. This",
	"               option can be given multiple times.",
	NULL
};

static int cmd_subvol_create(int argc, char **argv)
{
	int	retval, res, len;
	int	fddst = -1;
	char	*dupname = NULL;
	char	*dupdir = NULL;
	char	*newname;
	char	*dstdir;
	char	*dst;
	struct btrfs_qgroup_inherit *inherit = NULL;
	DIR	*dirstream = NULL;

	while (1) {
		int c = getopt(argc, argv, "c:i:");
		if (c < 0)
			break;

		switch (c) {
		case 'c':
			res = qgroup_inherit_add_copy(&inherit, optarg, 0);
			if (res) {
				retval = res;
				goto out;
			}
			break;
		case 'i':
			res = qgroup_inherit_add_group(&inherit, optarg);
			if (res) {
				retval = res;
				goto out;
			}
			break;
		default:
			usage(cmd_subvol_create_usage);
		}
	}

	if (check_argc_exact(argc - optind, 1))
		usage(cmd_subvol_create_usage);

	dst = argv[optind];

	retval = 1;	/* failure */
	res = test_isdir(dst);
	if (res < 0 && res != -ENOENT) {
		error("cannot access %s: %s", dst, strerror(-res));
		goto out;
	}
	if (res >= 0) {
		error("target path already exists: %s", dst);
		goto out;
	}

	dupname = strdup(dst);
	newname = basename(dupname);
	dupdir = strdup(dst);
	dstdir = dirname(dupdir);

	if (!test_issubvolname(newname)) {
		error("invalid subvolume name: %s", newname);
		goto out;
	}

	len = strlen(newname);
	if (len == 0 || len >= BTRFS_VOL_NAME_MAX) {
		error("subvolume name too long: %s", newname);
		goto out;
	}

	fddst = btrfs_open_dir(dstdir, &dirstream, 1);
	if (fddst < 0)
		goto out;

	printf("Create subvolume '%s/%s'\n", dstdir, newname);
	if (inherit) {
		struct btrfs_ioctl_vol_args_v2	args;

		memset(&args, 0, sizeof(args));
		strncpy_null(args.name, newname);
		args.flags |= BTRFS_SUBVOL_QGROUP_INHERIT;
		args.size = qgroup_inherit_size(inherit);
		args.qgroup_inherit = inherit;

		res = ioctl(fddst, BTRFS_IOC_SUBVOL_CREATE_V2, &args);
	} else {
		struct btrfs_ioctl_vol_args	args;

		memset(&args, 0, sizeof(args));
		strncpy_null(args.name, newname);

		res = ioctl(fddst, BTRFS_IOC_SUBVOL_CREATE, &args);
	}

	if (res < 0) {
		error("cannot create subvolume: %s", strerror(errno));
		goto out;
	}

	retval = 0;	/* success */
out:
	close_file_or_dir(fddst, dirstream);
	free(inherit);
	free(dupname);
	free(dupdir);

	return retval;
}

static int wait_for_commit(int fd)
{
	int ret;

	ret = ioctl(fd, BTRFS_IOC_START_SYNC, NULL);
	if (ret < 0)
		return ret;
	return ioctl(fd, BTRFS_IOC_WAIT_SYNC, NULL);
}

static const char * const cmd_subvol_delete_usage[] = {
	"btrfs subvolume delete [options] <subvolume> [<subvolume>...]",
	"Delete subvolume(s)",
	"Delete subvolumes from the filesystem. The corresponding directory",
	"is removed instantly but the data blocks are removed later.",
	"The deletion does not involve full commit by default due to",
	"performance reasons (as a consequence, the subvolume may appear again",
	"after a crash). Use one of the --commit options to wait until the",
	"operation is safely stored on the media.",
	"",
	"-c|--commit-after      wait for transaction commit at the end of the operation",
	"-C|--commit-each       wait for transaction commit after deleting each subvolume",
	"-v|--verbose           verbose output of operations",
	NULL
};

static int cmd_subvol_delete(int argc, char **argv)
{
	int res, ret = 0;
	int cnt;
	int fd = -1;
	struct btrfs_ioctl_vol_args	args;
	char	*dname, *vname, *cpath;
	char	*dupdname = NULL;
	char	*dupvname = NULL;
	char	*path;
	DIR	*dirstream = NULL;
	int verbose = 0;
	int commit_mode = 0;

	while (1) {
		int c;
		static const struct option long_options[] = {
			{"commit-after", no_argument, NULL, 'c'},  /* commit mode 1 */
			{"commit-each", no_argument, NULL, 'C'},  /* commit mode 2 */
			{"verbose", no_argument, NULL, 'v'},
			{NULL, 0, NULL, 0}
		};

		c = getopt_long(argc, argv, "cCv", long_options, NULL);
		if (c < 0)
			break;

		switch(c) {
		case 'c':
			commit_mode = 1;
			break;
		case 'C':
			commit_mode = 2;
			break;
		case 'v':
			verbose++;
			break;
		default:
			usage(cmd_subvol_delete_usage);
		}
	}

	if (check_argc_min(argc - optind, 1))
		usage(cmd_subvol_delete_usage);

	if (verbose > 0) {
		printf("Transaction commit: %s\n",
			!commit_mode ? "none (default)" :
			commit_mode == 1 ? "at the end" : "after each");
	}

	cnt = optind;

again:
	path = argv[cnt];

	res = test_issubvolume(path);
	if (res < 0) {
		error("cannot access subvolume %s: %s", path, strerror(-res));
		ret = 1;
		goto out;
	}
	if (!res) {
		error("not a subvolume: %s", path);
		ret = 1;
		goto out;
	}

	cpath = realpath(path, NULL);
	if (!cpath) {
		ret = errno;
		error("cannot find real path for '%s': %s",
			path, strerror(errno));
		goto out;
	}
	dupdname = strdup(cpath);
	dname = dirname(dupdname);
	dupvname = strdup(cpath);
	vname = basename(dupvname);
	free(cpath);

	fd = btrfs_open_dir(dname, &dirstream, 1);
	if (fd < 0) {
		ret = 1;
		goto out;
	}

	printf("Delete subvolume (%s): '%s/%s'\n",
		commit_mode == 2 || (commit_mode == 1 && cnt + 1 == argc)
		? "commit" : "no-commit", dname, vname);
	memset(&args, 0, sizeof(args));
	strncpy_null(args.name, vname);
	res = ioctl(fd, BTRFS_IOC_SNAP_DESTROY, &args);
	if(res < 0 ){
		error("cannot delete '%s/%s': %s", dname, vname,
			strerror(errno));
		ret = 1;
		goto out;
	}

	if (commit_mode == 1) {
		res = wait_for_commit(fd);
		if (res < 0) {
			error("unable to wait for commit after '%s': %s",
				path, strerror(errno));
			ret = 1;
		}
	}

out:
	free(dupdname);
	free(dupvname);
	dupdname = NULL;
	dupvname = NULL;
	cnt++;
	if (cnt < argc) {
		close_file_or_dir(fd, dirstream);
		/* avoid double free */
		fd = -1;
		dirstream = NULL;
		goto again;
	}

	if (commit_mode == 2 && fd != -1) {
		res = wait_for_commit(fd);
		if (res < 0) {
			error("unable to do final sync after deletion: %s",
				strerror(errno));
			ret = 1;
		}
	}
	close_file_or_dir(fd, dirstream);

	return ret;
}

/*
 * Naming of options:
 * - uppercase for filters and sort options
 * - lowercase for enabling specific items in the output
 */
static const char * const cmd_subvol_list_usage[] = {
	"btrfs subvolume list [options] [-G [+|-]value] [-C [+|-]value] "
	"[--sort=gen,ogen,rootid,path] <path>",
	"List subvolumes (and snapshots)",
	"",
	"-p           print parent ID",
	"-a           print all the subvolumes in the filesystem and",
	"             distinguish absolute and relative path with respect",
	"             to the given <path>",
	"-c           print the ogeneration of the subvolume",
	"-g           print the generation of the subvolume",
	"-o           print only subvolumes below specified path",
	"-u           print the uuid of subvolumes (and snapshots)",
	"-q           print the parent uuid of the snapshots",
	"-R           print the uuid of the received snapshots",
	"-t           print the result as a table",
	"-s           list snapshots only in the filesystem",
	"-r           list readonly subvolumes (including snapshots)",
	"-d           list deleted subvolumes that are not yet cleaned",
	"-G [+|-]value",
	"             filter the subvolumes by generation",
	"             (+value: >= value; -value: <= value; value: = value)",
	"-C [+|-]value",
	"             filter the subvolumes by ogeneration",
	"             (+value: >= value; -value: <= value; value: = value)",
	"--sort=gen,ogen,rootid,path",
	"             list the subvolume in order of gen, ogen, rootid or path",
	"             you also can add '+' or '-' in front of each items.",
	"             (+:ascending, -:descending, ascending default)",
	NULL,
};

static int cmd_subvol_list(int argc, char **argv)
{
	struct btrfs_list_filter_set *filter_set;
	struct btrfs_list_comparer_set *comparer_set;
	u64 flags = 0;
	int fd = -1;
	u64 top_id;
	int ret = -1, uerr = 0;
	char *subvol;
	int is_tab_result = 0;
	int is_list_all = 0;
	int is_only_in_path = 0;
	DIR *dirstream = NULL;

	filter_set = btrfs_list_alloc_filter_set();
	comparer_set = btrfs_list_alloc_comparer_set();

	while(1) {
		int c;
		static const struct option long_options[] = {
			{"sort", required_argument, NULL, 'S'},
			{NULL, 0, NULL, 0}
		};

		c = getopt_long(argc, argv,
				    "acdgopqsurRG:C:t", long_options, NULL);
		if (c < 0)
			break;

		switch(c) {
		case 'p':
			btrfs_list_setup_print_column(BTRFS_LIST_PARENT);
			break;
		case 'a':
			is_list_all = 1;
			break;
		case 'c':
			btrfs_list_setup_print_column(BTRFS_LIST_OGENERATION);
			break;
		case 'd':
			btrfs_list_setup_filter(&filter_set,
						BTRFS_LIST_FILTER_DELETED,
						0);
			break;
		case 'g':
			btrfs_list_setup_print_column(BTRFS_LIST_GENERATION);
			break;
		case 'o':
			is_only_in_path = 1;
			break;
		case 't':
			is_tab_result = 1;
			break;
		case 's':
			btrfs_list_setup_filter(&filter_set,
						BTRFS_LIST_FILTER_SNAPSHOT_ONLY,
						0);
			btrfs_list_setup_print_column(BTRFS_LIST_OGENERATION);
			btrfs_list_setup_print_column(BTRFS_LIST_OTIME);
			break;
		case 'u':
			btrfs_list_setup_print_column(BTRFS_LIST_UUID);
			break;
		case 'q':
			btrfs_list_setup_print_column(BTRFS_LIST_PUUID);
			break;
		case 'R':
			btrfs_list_setup_print_column(BTRFS_LIST_RUUID);
			break;
		case 'r':
			flags |= BTRFS_ROOT_SUBVOL_RDONLY;
			break;
		case 'G':
			btrfs_list_setup_print_column(BTRFS_LIST_GENERATION);
			ret = btrfs_list_parse_filter_string(optarg,
							&filter_set,
							BTRFS_LIST_FILTER_GEN);
			if (ret) {
				uerr = 1;
				goto out;
			}
			break;

		case 'C':
			btrfs_list_setup_print_column(BTRFS_LIST_OGENERATION);
			ret = btrfs_list_parse_filter_string(optarg,
							&filter_set,
							BTRFS_LIST_FILTER_CGEN);
			if (ret) {
				uerr = 1;
				goto out;
			}
			break;
		case 'S':
			ret = btrfs_list_parse_sort_string(optarg,
							   &comparer_set);
			if (ret) {
				uerr = 1;
				goto out;
			}
			break;

		default:
			uerr = 1;
			goto out;
		}
	}

	if (flags)
		btrfs_list_setup_filter(&filter_set, BTRFS_LIST_FILTER_FLAGS,
					flags);

	if (check_argc_exact(argc - optind, 1)) {
		uerr = 1;
		goto out;
	}

	subvol = argv[optind];
	fd = btrfs_open_dir(subvol, &dirstream, 1);
	if (fd < 0) {
		ret = -1;
		error("can't access '%s'", subvol);
		goto out;
	}

	ret = btrfs_list_get_path_rootid(fd, &top_id);
	if (ret) {
		error("can't get rootid for '%s'", subvol);
		goto out;
	}

	if (is_list_all)
		btrfs_list_setup_filter(&filter_set,
					BTRFS_LIST_FILTER_FULL_PATH,
					top_id);
	else if (is_only_in_path)
		btrfs_list_setup_filter(&filter_set,
					BTRFS_LIST_FILTER_TOPID_EQUAL,
					top_id);

	/* by default we shall print the following columns*/
	btrfs_list_setup_print_column(BTRFS_LIST_OBJECTID);
	btrfs_list_setup_print_column(BTRFS_LIST_GENERATION);
	btrfs_list_setup_print_column(BTRFS_LIST_TOP_LEVEL);
	btrfs_list_setup_print_column(BTRFS_LIST_PATH);

	if (is_tab_result)
		ret = btrfs_list_subvols_print(fd, filter_set, comparer_set,
				BTRFS_LIST_LAYOUT_TABLE,
				!is_list_all && !is_only_in_path, NULL);
	else
		ret = btrfs_list_subvols_print(fd, filter_set, comparer_set,
				BTRFS_LIST_LAYOUT_DEFAULT,
				!is_list_all && !is_only_in_path, NULL);

out:
	close_file_or_dir(fd, dirstream);
	if (filter_set)
		free(filter_set);
	if (comparer_set)
		free(comparer_set);
	if (uerr)
		usage(cmd_subvol_list_usage);
	return !!ret;
}

static const char * const cmd_subvol_snapshot_usage[] = {
	"btrfs subvolume snapshot [-r] [-i <qgroupid>] <source> <dest>|[<dest>/]<name>",
	"Create a snapshot of the subvolume",
	"Create a writable/readonly snapshot of the subvolume <source> with",
	"the name <name> in the <dest> directory.  If only <dest> is given,",
	"the subvolume will be named the basename of <source>.",
	"",
	"-r             create a readonly snapshot",
	"-i <qgroupid>  add the newly created snapshot to a qgroup. This",
	"               option can be given multiple times.",
	NULL
};

static int cmd_subvol_snapshot(int argc, char **argv)
{
	char	*subvol, *dst;
	int	res, retval;
	int	fd = -1, fddst = -1;
	int	len, readonly = 0;
	char	*dupname = NULL;
	char	*dupdir = NULL;
	char	*newname;
	char	*dstdir;
	struct btrfs_ioctl_vol_args_v2	args;
	struct btrfs_qgroup_inherit *inherit = NULL;
	DIR *dirstream1 = NULL, *dirstream2 = NULL;

	memset(&args, 0, sizeof(args));
	while (1) {
		int c = getopt(argc, argv, "c:i:r");
		if (c < 0)
			break;

		switch (c) {
		case 'c':
			res = qgroup_inherit_add_copy(&inherit, optarg, 0);
			if (res) {
				retval = res;
				goto out;
			}
			break;
		case 'i':
			res = qgroup_inherit_add_group(&inherit, optarg);
			if (res) {
				retval = res;
				goto out;
			}
			break;
		case 'r':
			readonly = 1;
			break;
		case 'x':
			res = qgroup_inherit_add_copy(&inherit, optarg, 1);
			if (res) {
				retval = res;
				goto out;
			}
			break;
		default:
			usage(cmd_subvol_snapshot_usage);
		}
	}

	if (check_argc_exact(argc - optind, 2))
		usage(cmd_subvol_snapshot_usage);

	subvol = argv[optind];
	dst = argv[optind + 1];

	retval = 1;	/* failure */
	res = test_issubvolume(subvol);
	if (res < 0) {
		error("cannot access subvolume %s: %s", subvol, strerror(-res));
		goto out;
	}
	if (!res) {
		error("not a subvolume: %s", subvol);
		goto out;
	}

	res = test_isdir(dst);
	if (res < 0 && res != -ENOENT) {
		error("cannot access %s: %s", dst, strerror(-res));
		goto out;
	}
	if (res == 0) {
		error("'%s' exists and it is not a directory", dst);
		goto out;
	}

	if (res > 0) {
		dupname = strdup(subvol);
		newname = basename(dupname);
		dstdir = dst;
	} else {
		dupname = strdup(dst);
		newname = basename(dupname);
		dupdir = strdup(dst);
		dstdir = dirname(dupdir);
	}

	if (!test_issubvolname(newname)) {
		error("invalid snapshot name '%s'", newname);
		goto out;
	}

	len = strlen(newname);
	if (len == 0 || len >= BTRFS_VOL_NAME_MAX) {
		error("snapshot name too long '%s'", newname);
		goto out;
	}

	fddst = btrfs_open_dir(dstdir, &dirstream1, 1);
	if (fddst < 0)
		goto out;

	fd = btrfs_open_dir(subvol, &dirstream2, 1);
	if (fd < 0)
		goto out;

	if (readonly) {
		args.flags |= BTRFS_SUBVOL_RDONLY;
		printf("Create a readonly snapshot of '%s' in '%s/%s'\n",
		       subvol, dstdir, newname);
	} else {
		printf("Create a snapshot of '%s' in '%s/%s'\n",
		       subvol, dstdir, newname);
	}

	args.fd = fd;
	if (inherit) {
		args.flags |= BTRFS_SUBVOL_QGROUP_INHERIT;
		args.size = qgroup_inherit_size(inherit);
		args.qgroup_inherit = inherit;
	}
	strncpy_null(args.name, newname);

	res = ioctl(fddst, BTRFS_IOC_SNAP_CREATE_V2, &args);

	if (res < 0) {
		error("cannot snapshot '%s': %s", subvol, strerror(errno));
		goto out;
	}

	retval = 0;	/* success */

out:
	close_file_or_dir(fddst, dirstream1);
	close_file_or_dir(fd, dirstream2);
	free(inherit);
	free(dupname);
	free(dupdir);

	return retval;
}

static const char * const cmd_subvol_get_default_usage[] = {
	"btrfs subvolume get-default <path>",
	"Get the default subvolume of a filesystem",
	NULL
};

static int cmd_subvol_get_default(int argc, char **argv)
{
	int fd = -1;
	int ret;
	char *subvol;
	struct btrfs_list_filter_set *filter_set;
	u64 default_id;
	DIR *dirstream = NULL;

	clean_args_no_options(argc, argv, cmd_subvol_get_default_usage);

	if (check_argc_exact(argc - optind, 1))
		usage(cmd_subvol_get_default_usage);

	subvol = argv[1];
	fd = btrfs_open_dir(subvol, &dirstream, 1);
	if (fd < 0)
		return 1;

	ret = btrfs_list_get_default_subvolume(fd, &default_id);
	if (ret) {
		error("failed to look up default subvolume: %s",
			strerror(errno));
		goto out;
	}

	ret = 1;
	if (default_id == 0) {
		error("'default' dir item not found");
		goto out;
	}

	/* no need to resolve roots if FS_TREE is default */
	if (default_id == BTRFS_FS_TREE_OBJECTID) {
		printf("ID 5 (FS_TREE)\n");
		ret = 0;
		goto out;
	}

	filter_set = btrfs_list_alloc_filter_set();
	btrfs_list_setup_filter(&filter_set, BTRFS_LIST_FILTER_ROOTID,
				default_id);

	/* by default we shall print the following columns*/
	btrfs_list_setup_print_column(BTRFS_LIST_OBJECTID);
	btrfs_list_setup_print_column(BTRFS_LIST_GENERATION);
	btrfs_list_setup_print_column(BTRFS_LIST_TOP_LEVEL);
	btrfs_list_setup_print_column(BTRFS_LIST_PATH);

	ret = btrfs_list_subvols_print(fd, filter_set, NULL,
		BTRFS_LIST_LAYOUT_DEFAULT, 1, NULL);

	if (filter_set)
		free(filter_set);
out:
	close_file_or_dir(fd, dirstream);
	return !!ret;
}

static const char * const cmd_subvol_set_default_usage[] = {
	"btrfs subvolume set-default <subvolid> <path>",
	"Set the default subvolume of a filesystem",
	NULL
};

static int cmd_subvol_set_default(int argc, char **argv)
{
	int	ret=0, fd, e;
	u64	objectid;
	char	*path;
	char	*subvolid;
	DIR	*dirstream = NULL;

	clean_args_no_options(argc, argv, cmd_subvol_set_default_usage);

	if (check_argc_exact(argc - optind, 2))
		usage(cmd_subvol_set_default_usage);

	subvolid = argv[optind];
	path = argv[optind + 1];

	objectid = arg_strtou64(subvolid);

	fd = btrfs_open_dir(path, &dirstream, 1);
	if (fd < 0)
		return 1;

	ret = ioctl(fd, BTRFS_IOC_DEFAULT_SUBVOL, &objectid);
	e = errno;
	close_file_or_dir(fd, dirstream);
	if (ret < 0) {
		error("unable to set a new default subvolume: %s",
			strerror(e));
		return 1;
	}
	return 0;
}

static const char * const cmd_subvol_find_new_usage[] = {
	"btrfs subvolume find-new <path> <lastgen>",
	"List the recently modified files in a filesystem",
	NULL
};

static int cmd_subvol_find_new(int argc, char **argv)
{
	int fd;
	int ret;
	char *subvol;
	u64 last_gen;
	DIR *dirstream = NULL;

	clean_args_no_options(argc, argv, cmd_subvol_find_new_usage);

	if (check_argc_exact(argc - optind, 2))
		usage(cmd_subvol_find_new_usage);

	subvol = argv[optind];
	last_gen = arg_strtou64(argv[optind + 1]);

	ret = test_issubvolume(subvol);
	if (ret < 0) {
		error("cannot access subvolume %s: %s", subvol, strerror(-ret));
		return 1;
	}
	if (!ret) {
		error("not a subvolume: %s", subvol);
		return 1;
	}

	fd = btrfs_open_dir(subvol, &dirstream, 1);
	if (fd < 0)
		return 1;

	ret = ioctl(fd, BTRFS_IOC_SYNC);
	if (ret < 0) {
		error("sync ioctl failed on '%s': %s",
			subvol, strerror(errno));
		close_file_or_dir(fd, dirstream);
		return 1;
	}

	ret = btrfs_list_find_updated_files(fd, 0, last_gen);
	close_file_or_dir(fd, dirstream);
	return !!ret;
}

static const char * const cmd_subvol_show_usage[] = {
	"btrfs subvolume show <subvol-path>",
	"Show more information of the subvolume",
	NULL
};

static int cmd_subvol_show(int argc, char **argv)
{
	struct root_info get_ri;
	struct btrfs_list_filter_set *filter_set = NULL;
	char tstr[256];
	char uuidparse[BTRFS_UUID_UNPARSED_SIZE];
	char *fullpath = NULL;
	char raw_prefix[] = "\t\t\t\t";
	int fd = -1;
	int ret = 1;
	DIR *dirstream1 = NULL;

	clean_args_no_options(argc, argv, cmd_subvol_show_usage);

	if (check_argc_exact(argc - optind, 1))
		usage(cmd_subvol_show_usage);

	memset(&get_ri, 0, sizeof(get_ri));
	fullpath = realpath(argv[optind], NULL);
	if (!fullpath) {
		error("cannot find real path for '%s': %s",
			argv[optind], strerror(errno));
		goto out;
	}

	ret = get_subvol_info(fullpath, &get_ri);
	if (ret == 2) {
		/*
		 * Since the top level btrfs was given don't
		 * take that as error
		 */
		printf("%s is toplevel subvolume\n", fullpath);
		ret = 0;
		goto out;
	}
	if (ret) {
		if (ret < 0) {
			error("Failed to get subvol info %s: %s\n",
					fullpath, strerror(-ret));
		} else {
			error("Failed to get subvol info %s: %d\n",
					fullpath, ret);
		}
		return ret;
	}

	/* print the info */
	printf("%s\n", fullpath);
	printf("\tName: \t\t\t%s\n", get_ri.name);

	if (uuid_is_null(get_ri.uuid))
		strcpy(uuidparse, "-");
	else
		uuid_unparse(get_ri.uuid, uuidparse);
	printf("\tUUID: \t\t\t%s\n", uuidparse);

	if (uuid_is_null(get_ri.puuid))
		strcpy(uuidparse, "-");
	else
		uuid_unparse(get_ri.puuid, uuidparse);
	printf("\tParent UUID: \t\t%s\n", uuidparse);

	if (uuid_is_null(get_ri.ruuid))
		strcpy(uuidparse, "-");
	else
		uuid_unparse(get_ri.ruuid, uuidparse);
	printf("\tReceived UUID: \t\t%s\n", uuidparse);

	if (get_ri.otime) {
		struct tm tm;

		localtime_r(&get_ri.otime, &tm);
		strftime(tstr, 256, "%Y-%m-%d %X %z", &tm);
	} else
		strcpy(tstr, "-");
	printf("\tCreation time: \t\t%s\n", tstr);

	printf("\tSubvolume ID: \t\t%llu\n", get_ri.root_id);
	printf("\tGeneration: \t\t%llu\n", get_ri.gen);
	printf("\tGen at creation: \t%llu\n", get_ri.ogen);
	printf("\tParent ID: \t\t%llu\n", get_ri.ref_tree);
	printf("\tTop level ID: \t\t%llu\n", get_ri.top_id);

	if (get_ri.flags & BTRFS_ROOT_SUBVOL_RDONLY)
		printf("\tFlags: \t\t\treadonly\n");
	else
		printf("\tFlags: \t\t\t-\n");

	/* print the snapshots of the given subvol if any*/
	printf("\tSnapshot(s):\n");
	filter_set = btrfs_list_alloc_filter_set();
	btrfs_list_setup_filter(&filter_set, BTRFS_LIST_FILTER_BY_PARENT,
				(u64)(unsigned long)get_ri.uuid);
	btrfs_list_setup_print_column(BTRFS_LIST_PATH);

	fd = open_file_or_dir(fullpath, &dirstream1);
	if (fd < 0) {
		fprintf(stderr, "ERROR: can't access '%s'\n", fullpath);
		goto out;
	}
	btrfs_list_subvols_print(fd, filter_set, NULL, BTRFS_LIST_LAYOUT_RAW,
			1, raw_prefix);

out:
	/* clean up */
	free(get_ri.path);
	free(get_ri.name);
	free(get_ri.full_path);
	free(filter_set);

	close_file_or_dir(fd, dirstream1);
	free(fullpath);
	return !!ret;
}

static const char * const cmd_subvol_sync_usage[] = {
	"btrfs subvolume sync <path> [<subvol-id>...]",
	"Wait until given subvolume(s) are completely removed from the filesystem.",
	"Wait until given subvolume(s) are completely removed from the filesystem",
	"after deletion.",
	"If no subvolume id is given, wait until all current deletion requests",
	"are completed, but do not wait for subvolumes deleted meanwhile.",
	"The status of subvolume ids is checked periodically.",
	"",
	"-s <N>       sleep N seconds between checks (default: 1)",
	NULL
};

#if 0
/*
 * If we're looking for any dead subvolume, take a shortcut and look
 * for any ORPHAN_ITEMs in the tree root
 */
static int fs_has_dead_subvolumes(int fd)
{
	int ret;
	struct btrfs_ioctl_search_args args;
	struct btrfs_ioctl_search_key *sk = &args.key;
	struct btrfs_ioctl_search_header sh;
	u64 min_subvolid = 0;

again:
	sk->tree_id = BTRFS_ROOT_TREE_OBJECTID;
	sk->min_objectid = BTRFS_ORPHAN_OBJECTID;
	sk->max_objectid = BTRFS_ORPHAN_OBJECTID;
	sk->min_type = BTRFS_ORPHAN_ITEM_KEY;
	sk->max_type = BTRFS_ORPHAN_ITEM_KEY;
	sk->min_offset = min_subvolid;
	sk->max_offset = (u64)-1;
	sk->min_transid = 0;
	sk->max_transid = (u64)-1;
	sk->nr_items = 1;

	ret = ioctl(fd, BTRFS_IOC_TREE_SEARCH, &args);
	if (ret < 0)
		return -errno;

	if (!sk->nr_items)
		return 0;

	memcpy(&sh, args.buf, sizeof(sh));
	min_subvolid = sh.offset;

	/*
	 * Verify that the root item is really there and we haven't hit
	 * a stale orphan
	 */
	sk->tree_id = BTRFS_ROOT_TREE_OBJECTID;
	sk->min_objectid = min_subvolid;
	sk->max_objectid = min_subvolid;
	sk->min_type = BTRFS_ROOT_ITEM_KEY;
	sk->max_type = BTRFS_ROOT_ITEM_KEY;
	sk->min_offset = 0;
	sk->max_offset = (u64)-1;
	sk->min_transid = 0;
	sk->max_transid = (u64)-1;
	sk->nr_items = 1;

	ret = ioctl(fd, BTRFS_IOC_TREE_SEARCH, &args);
	if (ret < 0)
		return -errno;

	/*
	 * Stale orphan, try the next one
	 */
	if (!sk->nr_items) {
		min_subvolid++;
		goto again;
	}

	return 1;
}
#endif

#define SUBVOL_ID_BATCH		1024

/*
 * Enumerate all dead subvolumes that exist in the filesystem.
 * Fill @ids and reallocate to bigger size if needed.
 */
static int enumerate_dead_subvols(int fd, u64 **ids)
{
	int ret;
	struct btrfs_ioctl_search_args args;
	struct btrfs_ioctl_search_key *sk = &args.key;
	int idx = 0;
	int count = 0;

	memset(&args, 0, sizeof(args));

	sk->tree_id = BTRFS_ROOT_TREE_OBJECTID;
	sk->min_objectid = BTRFS_ORPHAN_OBJECTID;
	sk->max_objectid = BTRFS_ORPHAN_OBJECTID;
	sk->min_type = BTRFS_ORPHAN_ITEM_KEY;
	sk->max_type = BTRFS_ORPHAN_ITEM_KEY;
	sk->min_offset = 0;
	sk->max_offset = (u64)-1;
	sk->min_transid = 0;
	sk->max_transid = (u64)-1;
	sk->nr_items = 4096;

	*ids = NULL;
	while (1) {
		struct btrfs_ioctl_search_header *sh;
		unsigned long off;
		int i;

		ret = ioctl(fd, BTRFS_IOC_TREE_SEARCH, &args);
		if (ret < 0)
			return -errno;

		if (!sk->nr_items)
			return idx;

		off = 0;
		for (i = 0; i < sk->nr_items; i++) {
			sh = (struct btrfs_ioctl_search_header*)(args.buf + off);
			off += sizeof(*sh);

			if (btrfs_search_header_type(sh)
			    == BTRFS_ORPHAN_ITEM_KEY) {
				if (idx >= count) {
					u64 *newids;

					count += SUBVOL_ID_BATCH;
					newids = (u64*)realloc(*ids,
							count * sizeof(u64));
					if (!newids)
						return -ENOMEM;
					*ids = newids;
				}
				(*ids)[idx] = btrfs_search_header_offset(sh);
				idx++;
			}
			off += btrfs_search_header_len(sh);

			sk->min_objectid = btrfs_search_header_objectid(sh);
			sk->min_type = btrfs_search_header_type(sh);
			sk->min_offset = btrfs_search_header_offset(sh);
		}
		if (sk->min_offset < (u64)-1)
			sk->min_offset++;
		else
			break;
		if (sk->min_type != BTRFS_ORPHAN_ITEM_KEY)
			break;
		if (sk->min_objectid != BTRFS_ORPHAN_OBJECTID)
			break;
	}

	return idx;
}

static int cmd_subvol_sync(int argc, char **argv)
{
	int fd = -1;
	int i;
	int ret = 1;
	DIR *dirstream = NULL;
	u64 *ids = NULL;
	int id_count;
	int sleep_interval = 1;

	while (1) {
		int c = getopt(argc, argv, "s:");

		if (c < 0)
			break;

		switch (c) {
		case 's':
			sleep_interval = atoi(optarg);
			if (sleep_interval < 1) {
				error("invalid sleep interval %s", optarg);
				ret = 1;
				goto out;
			}
			break;
		default:
			usage(cmd_subvol_sync_usage);
		}
	}

	if (check_argc_min(argc - optind, 1))
		usage(cmd_subvol_sync_usage);

	fd = btrfs_open_dir(argv[optind], &dirstream, 1);
	if (fd < 0) {
		ret = 1;
		goto out;
	}
	optind++;

	id_count = argc - optind;
	if (!id_count) {
		id_count = enumerate_dead_subvols(fd, &ids);
		if (id_count < 0) {
			error("can't enumerate dead subvolumes: %s",
					strerror(-id_count));
			ret = 1;
			goto out;
		}
		if (id_count == 0) {
			ret = 0;
			goto out;
		}
	} else {
		ids = (u64*)malloc(id_count * sizeof(u64));
		if (!ids) {
			error("not enough memory");
			ret = 1;
			goto out;
		}

		for (i = 0; i < id_count; i++) {
			u64 id;
			const char *arg;

			arg = argv[optind + i];
			errno = 0;
			id = strtoull(arg, NULL, 10);
			if (errno < 0) {
				error("unrecognized subvolume id %s", arg);
				ret = 1;
				goto out;
			}
			if (id < BTRFS_FIRST_FREE_OBJECTID
					|| id > BTRFS_LAST_FREE_OBJECTID) {
				error("subvolume id %s out of range\n", arg);
				ret = 1;
				goto out;
			}
			ids[i] = id;
		}
	}

	ret = wait_for_subvolume_cleaning(fd, id_count, ids, sleep_interval);

out:
	free(ids);
	close_file_or_dir(fd, dirstream);

	return !!ret;
}

static const char subvolume_cmd_group_info[] =
"manage subvolumes: create, delete, list, etc";

const struct cmd_group subvolume_cmd_group = {
	subvolume_cmd_group_usage, subvolume_cmd_group_info, {
		{ "create", cmd_subvol_create, cmd_subvol_create_usage, NULL, 0 },
		{ "delete", cmd_subvol_delete, cmd_subvol_delete_usage, NULL, 0 },
		{ "list", cmd_subvol_list, cmd_subvol_list_usage, NULL, 0 },
		{ "snapshot", cmd_subvol_snapshot, cmd_subvol_snapshot_usage,
			NULL, 0 },
		{ "get-default", cmd_subvol_get_default,
			cmd_subvol_get_default_usage, NULL, 0 },
		{ "set-default", cmd_subvol_set_default,
			cmd_subvol_set_default_usage, NULL, 0 },
		{ "find-new", cmd_subvol_find_new, cmd_subvol_find_new_usage,
			NULL, 0 },
		{ "show", cmd_subvol_show, cmd_subvol_show_usage, NULL, 0 },
		{ "sync", cmd_subvol_sync, cmd_subvol_sync_usage, NULL, 0 },
		NULL_CMD_STRUCT
	}
};

int cmd_subvolume(int argc, char **argv)
{
	return handle_command_group(&subvolume_cmd_group, argc, argv);
}
