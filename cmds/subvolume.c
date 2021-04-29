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

#include <inttypes.h>
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

#include <btrfsutil.h>

#include "kerncompat.h"
#include "ioctl.h"
#include "qgroup.h"

#include "kernel-shared/ctree.h"
#include "cmds/commands.h"
#include "common/utils.h"
#include "btrfs-list.h"
#include "common/help.h"
#include "common/path-utils.h"
#include "common/device-scan.h"
#include "common/open-utils.h"
#include "common/units.h"

static int wait_for_subvolume_cleaning(int fd, size_t count, uint64_t *ids,
				       int sleep_interval)
{
	size_t i;
	enum btrfs_util_error err;

	while (1) {
		int clean = 1;

		for (i = 0; i < count; i++) {
			if (!ids[i])
				continue;
			err = btrfs_util_subvolume_info_fd(fd, ids[i], NULL);
			if (err == BTRFS_UTIL_ERROR_SUBVOLUME_NOT_FOUND) {
				printf("Subvolume id %" PRIu64 " is gone\n",
				       ids[i]);
				ids[i] = 0;
			} else if (err) {
				error_btrfs_util(err);
				return -errno;
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
	HELPINFO_INSERT_GLOBALS,
	HELPINFO_INSERT_QUIET,
	NULL
};

static int cmd_subvol_create(const struct cmd_struct *cmd,
			     int argc, char **argv)
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

	optind = 0;
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
			usage_unknown_option(cmd, argv);
		}
	}

	if (check_argc_exact(argc - optind, 1)) {
		retval = 1;
		goto out;
	}

	dst = argv[optind];

	retval = 1;	/* failure */
	res = path_is_dir(dst);
	if (res < 0 && res != -ENOENT) {
		errno = -res;
		error("cannot access %s: %m", dst);
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
	if (len > BTRFS_VOL_NAME_MAX) {
		error("subvolume name too long: %s", newname);
		goto out;
	}

	fddst = btrfs_open_dir(dstdir, &dirstream, 1);
	if (fddst < 0)
		goto out;

	pr_verbose(MUST_LOG, "Create subvolume '%s/%s'\n", dstdir, newname);
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
		error("cannot create subvolume: %m");
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
static DEFINE_SIMPLE_COMMAND(subvol_create, "create");

static int wait_for_commit(int fd)
{
	enum btrfs_util_error err;
	uint64_t transid;

	err = btrfs_util_start_sync_fd(fd, &transid);
	if (err)
		return -1;

	err = btrfs_util_wait_sync_fd(fd, transid);
	if (err)
		return -1;

	return 0;
}

static const char * const cmd_subvol_delete_usage[] = {
	"btrfs subvolume delete [options] <subvolume> [<subvolume>...]\n"
	"btrfs subvolume delete [options] -i|--subvolid <subvolid> <path>",
	"Delete subvolume(s)",
	"Delete subvolumes from the filesystem, specified by a path or id. The",
	"corresponding directory is removed instantly but the data blocks are",
	"removed later.",
	"The deletion does not involve full commit by default due to",
	"performance reasons (as a consequence, the subvolume may appear again",
	"after a crash). Use one of the --commit options to wait until the",
	"operation is safely stored on the media.",
	"",
	"-c|--commit-after      wait for transaction commit at the end of the operation",
	"-C|--commit-each       wait for transaction commit after deleting each subvolume",
	"-i|--subvolid          subvolume id of the to be removed subvolume",
	"-v|--verbose           deprecated, alias for global -v option",
	HELPINFO_INSERT_GLOBALS,
	HELPINFO_INSERT_VERBOSE,
	HELPINFO_INSERT_QUIET,
	NULL
};

static int cmd_subvol_delete(const struct cmd_struct *cmd,
			     int argc, char **argv)
{
	int res, ret = 0;
	int cnt;
	int fd = -1;
	char	*dname, *vname, *cpath;
	char	*dupdname = NULL;
	char	*dupvname = NULL;
	char	*path = NULL;
	DIR	*dirstream = NULL;
	int commit_mode = 0;
	u8 fsid[BTRFS_FSID_SIZE];
	u64 subvolid = 0;
	char uuidbuf[BTRFS_UUID_UNPARSED_SIZE];
	char full_subvolpath[BTRFS_SUBVOL_NAME_MAX];
	struct seen_fsid *seen_fsid_hash[SEEN_FSID_HASH_SIZE] = { NULL, };
	enum { COMMIT_AFTER = 1, COMMIT_EACH = 2 };
	enum btrfs_util_error err;
	uint64_t default_subvol_id = 0, target_subvol_id = 0;

	optind = 0;
	while (1) {
		int c;
		static const struct option long_options[] = {
			{"commit-after", no_argument, NULL, 'c'},
			{"commit-each", no_argument, NULL, 'C'},
			{"subvolid", required_argument, NULL, 'i'},
			{"verbose", no_argument, NULL, 'v'},
			{NULL, 0, NULL, 0}
		};

		c = getopt_long(argc, argv, "cCi:v", long_options, NULL);
		if (c < 0)
			break;

		switch(c) {
		case 'c':
			commit_mode = COMMIT_AFTER;
			break;
		case 'C':
			commit_mode = COMMIT_EACH;
			break;
		case 'i':
			subvolid = arg_strtou64(optarg);
			break;
		case 'v':
			bconf_be_verbose();
			break;
		default:
			usage_unknown_option(cmd, argv);
		}
	}

	if (check_argc_min(argc - optind, 1))
		return 1;

	/* When using --subvolid, ensure that we have only one argument */
	if (subvolid > 0 && check_argc_exact(argc - optind, 1))
		return 1;

	pr_verbose(1, "Transaction commit: %s\n",
		   !commit_mode ? "none (default)" :
		   commit_mode == COMMIT_AFTER ? "at the end" : "after each");

	cnt = optind;

	/* Check the following syntax: subvolume delete --subvolid <subvolid> <path> */
	if (subvolid > 0) {
		char *subvol;

		path = argv[cnt];
		err = btrfs_util_subvolume_path(path, subvolid, &subvol);
		if (err) {
			error_btrfs_util(err);
			ret = 1;
			goto out;
		}

		/* Build new path using the volume name found */
		sprintf(full_subvolpath, "%s/%s", path, subvol);
		free(subvol);
	}

again:
	path = argv[cnt];

	err = btrfs_util_is_subvolume(path);
	if (err) {
		error_btrfs_util(err);
		ret = 1;
		goto out;
	}

	cpath = realpath(path, NULL);
	if (!cpath) {
		ret = errno;
		error("cannot find real path for '%s': %m", path);
		goto out;
	}
	dupdname = strdup(cpath);
	dname = dirname(dupdname);
	dupvname = strdup(cpath);
	vname = basename(dupvname);
	free(cpath);

	/* When subvolid is passed, <path> will point to the mount point */
	if (subvolid > 0)
		dname = dupvname;

	fd = btrfs_open_dir(dname, &dirstream, 1);
	if (fd < 0) {
		ret = 1;
		goto out;
	}

	err = btrfs_util_get_default_subvolume_fd(fd, &default_subvol_id);
	if (err) {
		warning("cannot read default subvolume id: %m");
		default_subvol_id = 0;
	}

	if (subvolid > 0) {
		target_subvol_id = subvolid;
	} else {
		err = btrfs_util_subvolume_id(path, &target_subvol_id);
		if (err) {
			ret = 1;
			goto out;
		}
	}

	if (target_subvol_id == default_subvol_id) {
		warning("not deleting default subvolume id %llu '%s%s%s'",
				(u64)default_subvol_id,
				(subvolid == 0 ? dname	: ""),
				(subvolid == 0 ? "/"	: ""),
				(subvolid == 0 ? vname	: full_subvolpath));
		ret = 1;
		goto out;
	}

	pr_verbose(MUST_LOG, "Delete subvolume (%s): ",
		commit_mode == COMMIT_EACH ||
		(commit_mode == COMMIT_AFTER && cnt + 1 == argc) ?
		"commit" : "no-commit");

	if (subvolid == 0)
		pr_verbose(MUST_LOG, "'%s/%s'\n", dname, vname);
	else
		pr_verbose(MUST_LOG, "'%s'\n", full_subvolpath);

	if (subvolid == 0)
		err = btrfs_util_delete_subvolume_fd(fd, vname, 0);
	else
		err = btrfs_util_delete_subvolume_by_id_fd(fd, subvolid);
	if (err) {
		int saved_errno = errno;

		error_btrfs_util(err);
		if (saved_errno == EPERM)
			warning("deletion failed with EPERM, send may be in progress");
		ret = 1;
		goto out;
	}

	if (commit_mode == COMMIT_EACH) {
		res = wait_for_commit(fd);
		if (res < 0) {
			error("unable to wait for commit after '%s': %m", path);
			ret = 1;
		}
	} else if (commit_mode == COMMIT_AFTER) {
		res = get_fsid(dname, fsid, 0);
		if (res < 0) {
			errno = -res;
			error("unable to get fsid for '%s': %m", path);
			error(
			"delete succeeded but commit may not be done in the end");
			ret = 1;
			goto out;
		}

		if (add_seen_fsid(fsid, seen_fsid_hash, fd, dirstream) == 0) {
			uuid_unparse(fsid, uuidbuf);
			pr_verbose(1, "  new fs is found for '%s', fsid: %s\n",
				   path, uuidbuf);
			/*
			 * This is the first time a subvolume on this
			 * filesystem is deleted, keep fd in order to issue
			 * SYNC ioctl in the end
			 */
			goto keep_fd;
		}
	}

out:
	close_file_or_dir(fd, dirstream);
keep_fd:
	fd = -1;
	dirstream = NULL;
	free(dupdname);
	free(dupvname);
	dupdname = NULL;
	dupvname = NULL;
	cnt++;
	if (cnt < argc)
		goto again;

	if (commit_mode == COMMIT_AFTER) {
		int slot;

		/*
		 * Traverse seen_fsid_hash and issue SYNC ioctl on each
		 * filesystem
		 */
		for (slot = 0; slot < SEEN_FSID_HASH_SIZE; slot++) {
			struct seen_fsid *seen = seen_fsid_hash[slot];

			while (seen) {
				res = wait_for_commit(seen->fd);
				if (res < 0) {
					uuid_unparse(seen->fsid, uuidbuf);
					error(
			"unable to do final sync after deletion: %m, fsid: %s",
						uuidbuf);
					ret = 1;
				} else {
					uuid_unparse(seen->fsid, uuidbuf);
					pr_verbose(1,
					   "final sync is done for fsid: %s\n",
						   uuidbuf);
				}
				seen = seen->next;
			}
		}
		/* fd will also be closed in free_seen_fsid */
		free_seen_fsid(seen_fsid_hash);
	}

	return ret;
}
static DEFINE_SIMPLE_COMMAND(subvol_delete, "delete");

/*
 * Naming of options:
 * - uppercase for filters and sort options
 * - lowercase for enabling specific items in the output
 */
static const char * const cmd_subvol_list_usage[] = {
	"btrfs subvolume list [options] <path>",
	"List subvolumes and snapshots in the filesystem.",
	"",
	"Path filtering:",
	"-o           print only subvolumes below specified path",
	"-a           print all the subvolumes in the filesystem and",
	"             distinguish absolute and relative path with respect",
	"             to the given <path>",
	"",
	"Field selection:",
	"-p           print parent ID",
	"-c           print the ogeneration of the subvolume",
	"-g           print the generation of the subvolume",
	"-u           print the uuid of subvolumes (and snapshots)",
	"-q           print the parent uuid of the snapshots",
	"-R           print the uuid of the received snapshots",
	"",
	"Type filtering:",
	"-s           list only snapshots",
	"-r           list readonly subvolumes (including snapshots)",
	"-d           list deleted subvolumes that are not yet cleaned",
	"",
	"Other:",
	"-t           print the result as a table",
	"",
	"Sorting:",
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

static int cmd_subvol_list(const struct cmd_struct *cmd, int argc, char **argv)
{
	struct btrfs_list_filter_set *filter_set;
	struct btrfs_list_comparer_set *comparer_set;
	u64 flags = 0;
	int fd = -1;
	u64 top_id;
	int ret = -1, uerr = 0;
	char *subvol;
	int is_list_all = 0;
	int is_only_in_path = 0;
	DIR *dirstream = NULL;
	enum btrfs_list_layout layout = BTRFS_LIST_LAYOUT_DEFAULT;

	filter_set = btrfs_list_alloc_filter_set();
	comparer_set = btrfs_list_alloc_comparer_set();

	optind = 0;
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
			layout = BTRFS_LIST_LAYOUT_TABLE;
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

	if (check_argc_exact(argc - optind, 1))
		goto out;

	subvol = argv[optind];
	fd = btrfs_open_dir(subvol, &dirstream, 1);
	if (fd < 0) {
		ret = -1;
		error("can't access '%s'", subvol);
		goto out;
	}

	if (flags)
		btrfs_list_setup_filter(&filter_set, BTRFS_LIST_FILTER_FLAGS,
					flags);

	ret = btrfs_list_get_path_rootid(fd, &top_id);
	if (ret)
		goto out;

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

	ret = btrfs_list_subvols_print(fd, filter_set, comparer_set,
			layout, !is_list_all && !is_only_in_path, NULL);

out:
	close_file_or_dir(fd, dirstream);
	if (filter_set)
		free(filter_set);
	if (comparer_set)
		free(comparer_set);
	if (uerr)
		usage(cmd);
	return !!ret;
}
static DEFINE_SIMPLE_COMMAND(subvol_list, "list");

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
	HELPINFO_INSERT_GLOBALS,
	HELPINFO_INSERT_QUIET,
	NULL
};

static int cmd_subvol_snapshot(const struct cmd_struct *cmd,
			       int argc, char **argv)
{
	char	*subvol, *dst;
	int	res, retval;
	int	fd = -1, fddst = -1;
	int	len, readonly = 0;
	char	*dupname = NULL;
	char	*dupdir = NULL;
	char	*newname;
	char	*dstdir;
	enum btrfs_util_error err;
	struct btrfs_ioctl_vol_args_v2	args;
	struct btrfs_qgroup_inherit *inherit = NULL;
	DIR *dirstream1 = NULL, *dirstream2 = NULL;

	memset(&args, 0, sizeof(args));
	optind = 0;
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
			usage_unknown_option(cmd, argv);
		}
	}

	if (check_argc_exact(argc - optind, 2)) {
		retval = 1;
		goto out;
	}

	subvol = argv[optind];
	dst = argv[optind + 1];

	retval = 1;	/* failure */
	err = btrfs_util_is_subvolume(subvol);
	if (err) {
		error_btrfs_util(err);
		goto out;
	}

	res = path_is_dir(dst);
	if (res < 0 && res != -ENOENT) {
		errno = -res;
		error("cannot access %s: %m", dst);
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
	if (len > BTRFS_VOL_NAME_MAX) {
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
		pr_verbose(MUST_LOG,
			   "Create a readonly snapshot of '%s' in '%s/%s'\n",
			   subvol, dstdir, newname);
	} else {
		pr_verbose(MUST_LOG,
			   "Create a snapshot of '%s' in '%s/%s'\n",
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
		error("cannot snapshot '%s': %m", subvol);
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
static DEFINE_SIMPLE_COMMAND(subvol_snapshot, "snapshot");

static const char * const cmd_subvol_get_default_usage[] = {
	"btrfs subvolume get-default <path>",
	"Get the default subvolume of a filesystem",
	NULL
};

static int cmd_subvol_get_default(const struct cmd_struct *cmd,
				  int argc, char **argv)
{
	int fd = -1;
	int ret = 1;
	uint64_t default_id;
	DIR *dirstream = NULL;
	enum btrfs_util_error err;
	struct btrfs_util_subvolume_info subvol;
	char *path;

	clean_args_no_options(cmd, argc, argv);

	if (check_argc_exact(argc - optind, 1))
		return 1;

	fd = btrfs_open_dir(argv[1], &dirstream, 1);
	if (fd < 0)
		return 1;

	err = btrfs_util_get_default_subvolume_fd(fd, &default_id);
	if (err) {
		error_btrfs_util(err);
		goto out;
	}

	/* no need to resolve roots if FS_TREE is default */
	if (default_id == BTRFS_FS_TREE_OBJECTID) {
		printf("ID 5 (FS_TREE)\n");
		ret = 0;
		goto out;
	}

	err = btrfs_util_subvolume_info_fd(fd, default_id, &subvol);
	if (err) {
		error_btrfs_util(err);
		goto out;
	}

	err = btrfs_util_subvolume_path_fd(fd, default_id, &path);
	if (err) {
		error_btrfs_util(err);
		goto out;
	}

	printf("ID %" PRIu64 " gen %" PRIu64 " top level %" PRIu64 " path %s\n",
	       subvol.id, subvol.generation, subvol.parent_id, path);

	free(path);

	ret = 0;
out:
	close_file_or_dir(fd, dirstream);
	return ret;
}
static DEFINE_SIMPLE_COMMAND(subvol_get_default, "get-default");

static const char * const cmd_subvol_set_default_usage[] = {
	"btrfs subvolume set-default <subvolume>\n"
	"btrfs subvolume set-default <subvolid> <path>",
	"Set the default subvolume of the filesystem mounted as default.",
	"The subvolume can be specified by its path,",
	"or the pair of subvolume id and path to the filesystem.",
	NULL
};

static int cmd_subvol_set_default(const struct cmd_struct *cmd,
				  int argc, char **argv)
{
	u64 objectid;
	char *path;
	enum btrfs_util_error err;

	clean_args_no_options(cmd, argc, argv);

	if (check_argc_min(argc - optind, 1) ||
			check_argc_max(argc - optind, 2))
		return 1;

	if (argc - optind == 1) {
		/* path to the subvolume is specified */
		objectid = 0;
		path = argv[optind];
	} else {
		/* subvol id and path to the filesystem are specified */
		objectid = arg_strtou64(argv[optind]);
		/*
		 * To avoid confusion with the case above inside libbtrfsutil,
		 * we must set the toplevel as default manually, same what
		 * would kernel do.
		 */
		if (objectid == 0)
			objectid = BTRFS_FS_TREE_OBJECTID;
		path = argv[optind + 1];
	}

	err = btrfs_util_set_default_subvolume(path, objectid);
	if (err) {
		error_btrfs_util(err);
		return 1;
	}
	return 0;
}
static DEFINE_SIMPLE_COMMAND(subvol_set_default, "set-default");

static const char * const cmd_subvol_find_new_usage[] = {
	"btrfs subvolume find-new <path> <lastgen>",
	"List the recently modified files in a filesystem",
	NULL
};

static int cmd_subvol_find_new(const struct cmd_struct *cmd,
			       int argc, char **argv)
{
	int fd;
	int ret;
	char *subvol;
	u64 last_gen;
	DIR *dirstream = NULL;
	enum btrfs_util_error err;

	clean_args_no_options(cmd, argc, argv);

	if (check_argc_exact(argc - optind, 2))
		return 1;

	subvol = argv[optind];
	last_gen = arg_strtou64(argv[optind + 1]);

	err = btrfs_util_is_subvolume(subvol);
	if (err) {
		error_btrfs_util(err);
		return 1;
	}

	fd = btrfs_open_dir(subvol, &dirstream, 1);
	if (fd < 0)
		return 1;

	err = btrfs_util_sync_fd(fd);
	if (err) {
		error_btrfs_util(err);
		close_file_or_dir(fd, dirstream);
		return 1;
	}

	ret = btrfs_list_find_updated_files(fd, 0, last_gen);
	close_file_or_dir(fd, dirstream);
	return !!ret;
}
static DEFINE_SIMPLE_COMMAND(subvol_find_new, "find-new");

static const char * const cmd_subvol_show_usage[] = {
	"btrfs subvolume show [options] <path>",
	"Show more information about the subvolume (UUIDs, generations, times, snapshots)",
	"Show more information about the subvolume (UUIDs, generations, times, snapshots).",
	"The subvolume can be specified by path, or by root id or UUID that are",
	"looked up relative to the given path",
	"",
	"-r|--rootid ID       root id of the subvolume",
	"-u|--uuid UUID       UUID of the subvolum",
	HELPINFO_UNITS_SHORT_LONG,
	NULL
};

static int cmd_subvol_show(const struct cmd_struct *cmd, int argc, char **argv)
{
	char tstr[256];
	char uuidparse[BTRFS_UUID_UNPARSED_SIZE];
	char *fullpath = NULL;
	int fd = -1;
	int ret = 1;
	DIR *dirstream1 = NULL;
	int by_rootid = 0;
	int by_uuid = 0;
	u64 rootid_arg = 0;
	u8 uuid_arg[BTRFS_UUID_SIZE];
	struct btrfs_util_subvolume_iterator *iter;
	struct btrfs_util_subvolume_info subvol;
	char *subvol_path = NULL;
	enum btrfs_util_error err;
	struct btrfs_qgroup_stats stats;
	unsigned int unit_mode;

	unit_mode = get_unit_mode_from_arg(&argc, argv, 1);

	optind = 0;
	while (1) {
		int c;
		static const struct option long_options[] = {
			{ "rootid", required_argument, NULL, 'r'},
			{ "uuid", required_argument, NULL, 'u'},
			{ NULL, 0, NULL, 0 }
		};

		c = getopt_long(argc, argv, "r:u:", long_options, NULL);
		if (c < 0)
			break;

		switch (c) {
		case 'r':
			rootid_arg = arg_strtou64(optarg);
			by_rootid = 1;
			break;
		case 'u':
			uuid_parse(optarg, uuid_arg);
			by_uuid = 1;
			break;
		default:
			usage_unknown_option(cmd, argv);
		}
	}

	if (check_argc_exact(argc - optind, 1))
		return 1;

	if (by_rootid && by_uuid) {
		error(
		"options --rootid and --uuid cannot be used at the same time");
		usage(cmd);
	}

	fullpath = realpath(argv[optind], NULL);
	if (!fullpath) {
		error("cannot find real path for '%s': %m", argv[optind]);
		goto out;
	}

	fd = open_file_or_dir(fullpath, &dirstream1);
	if (fd < 0) {
		error("can't access '%s'", fullpath);
		goto out;
	}

	if (by_uuid) {
		err = btrfs_util_create_subvolume_iterator_fd(fd,
							      BTRFS_FS_TREE_OBJECTID,
							      0, &iter);
		if (err) {
			error_btrfs_util(err);
			goto out;
		}

		for (;;) {
			err = btrfs_util_subvolume_iterator_next_info(iter,
								      &subvol_path,
								      &subvol);
			if (err == BTRFS_UTIL_ERROR_STOP_ITERATION) {
				uuid_unparse(uuid_arg, uuidparse);
				error("can't find uuid '%s' on '%s'", uuidparse,
				      fullpath);
				btrfs_util_destroy_subvolume_iterator(iter);
				goto out;
			} else if (err) {
				error_btrfs_util(err);
				btrfs_util_destroy_subvolume_iterator(iter);
				goto out;
			}

			if (uuid_compare(subvol.uuid, uuid_arg) == 0)
				break;

			free(subvol_path);
			subvol_path = NULL;
		}
		btrfs_util_destroy_subvolume_iterator(iter);
	} else {
		/*
		 * If !by_rootid, rootid_arg = 0, which means find the
		 * subvolume ID of the fd and use that.
		 */
		err = btrfs_util_subvolume_info_fd(fd, rootid_arg, &subvol);
		if (err) {
			error_btrfs_util(err);
			goto out;
		}

		err = btrfs_util_subvolume_path_fd(fd, subvol.id, &subvol_path);
		if (err) {
			error_btrfs_util(err);
			goto out;
		}

	}

	/* print the info */
	printf("%s\n", subvol.id == BTRFS_FS_TREE_OBJECTID ? "/" : subvol_path);
	printf("\tName: \t\t\t%s\n",
	       (subvol.id == BTRFS_FS_TREE_OBJECTID ? "<FS_TREE>" :
		basename(subvol_path)));

	if (uuid_is_null(subvol.uuid))
		strcpy(uuidparse, "-");
	else
		uuid_unparse(subvol.uuid, uuidparse);
	printf("\tUUID: \t\t\t%s\n", uuidparse);

	if (uuid_is_null(subvol.parent_uuid))
		strcpy(uuidparse, "-");
	else
		uuid_unparse(subvol.parent_uuid, uuidparse);
	printf("\tParent UUID: \t\t%s\n", uuidparse);

	if (uuid_is_null(subvol.received_uuid))
		strcpy(uuidparse, "-");
	else
		uuid_unparse(subvol.received_uuid, uuidparse);
	printf("\tReceived UUID: \t\t%s\n", uuidparse);

	if (subvol.otime.tv_sec) {
		struct tm tm;

		localtime_r(&subvol.otime.tv_sec, &tm);
		strftime(tstr, 256, "%Y-%m-%d %X %z", &tm);
	} else
		strcpy(tstr, "-");
	printf("\tCreation time: \t\t%s\n", tstr);

	printf("\tSubvolume ID: \t\t%" PRIu64 "\n", subvol.id);
	printf("\tGeneration: \t\t%" PRIu64 "\n", subvol.generation);
	printf("\tGen at creation: \t%" PRIu64 "\n", subvol.otransid);
	printf("\tParent ID: \t\t%" PRIu64 "\n", subvol.parent_id);
	printf("\tTop level ID: \t\t%" PRIu64 "\n", subvol.parent_id);

	if (subvol.flags & BTRFS_ROOT_SUBVOL_RDONLY)
		printf("\tFlags: \t\t\treadonly\n");
	else
		printf("\tFlags: \t\t\t-\n");

	/* print the snapshots of the given subvol if any*/
	printf("\tSnapshot(s):\n");

	err = btrfs_util_create_subvolume_iterator_fd(fd,
						      BTRFS_FS_TREE_OBJECTID, 0,
						      &iter);

	for (;;) {
		struct btrfs_util_subvolume_info subvol2;
		char *path;

		err = btrfs_util_subvolume_iterator_next_info(iter, &path, &subvol2);
		if (err == BTRFS_UTIL_ERROR_STOP_ITERATION) {
			break;
		} else if (err) {
			error_btrfs_util(err);
			btrfs_util_destroy_subvolume_iterator(iter);
			goto out;
		}

		if (uuid_compare(subvol2.parent_uuid, subvol.uuid) == 0)
			printf("\t\t\t\t%s\n", path);

		free(path);
	}
	btrfs_util_destroy_subvolume_iterator(iter);

	ret = btrfs_qgroup_query(fd, subvol.id, &stats);
	if (ret == -ENOTTY) {
		/* Quotas not enabled */
		ret = 0;
		goto out;
	}
	if (ret == -ENOTTY) {
		/* Quota information not available, not fatal */
		printf("\tQuota group:\t\tn/a\n");
		ret = 0;
		goto out;
	}

	if (ret) {
		fprintf(stderr, "ERROR: quota query failed: %m");
		goto out;
	}

	printf("\tQuota group:\t\t0/%" PRIu64 "\n", subvol.id);
	fflush(stdout);

	printf("\t  Limit referenced:\t%s\n",
			stats.limit.max_referenced == 0 ? "-" :
			pretty_size_mode(stats.limit.max_referenced, unit_mode));
	printf("\t  Limit exclusive:\t%s\n",
			stats.limit.max_exclusive == 0 ? "-" :
			pretty_size_mode(stats.limit.max_exclusive, unit_mode));
	printf("\t  Usage referenced:\t%s\n",
			pretty_size_mode(stats.info.referenced, unit_mode));
	printf("\t  Usage exclusive:\t%s\n",
			pretty_size_mode(stats.info.exclusive, unit_mode));

out:
	free(subvol_path);
	close_file_or_dir(fd, dirstream1);
	free(fullpath);
	return !!ret;
}
static DEFINE_SIMPLE_COMMAND(subvol_show, "show");

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

static int cmd_subvol_sync(const struct cmd_struct *cmd, int argc, char **argv)
{
	int fd = -1;
	int ret = 1;
	DIR *dirstream = NULL;
	uint64_t *ids = NULL;
	size_t id_count, i;
	int sleep_interval = 1;
	enum btrfs_util_error err;

	optind = 0;
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
			usage_unknown_option(cmd, argv);
		}
	}

	if (check_argc_min(argc - optind, 1))
		return 1;

	fd = btrfs_open_dir(argv[optind], &dirstream, 1);
	if (fd < 0) {
		ret = 1;
		goto out;
	}
	optind++;

	id_count = argc - optind;
	if (!id_count) {
		err = btrfs_util_deleted_subvolumes_fd(fd, &ids, &id_count);
		if (err) {
			error_btrfs_util(err);
			ret = 1;
			goto out;
		}
		if (id_count == 0) {
			ret = 0;
			goto out;
		}
	} else {
		ids = malloc(id_count * sizeof(uint64_t));
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
			if (errno) {
				error("unrecognized subvolume id %s", arg);
				ret = 1;
				goto out;
			}
			if (id < BTRFS_FIRST_FREE_OBJECTID ||
			    id > BTRFS_LAST_FREE_OBJECTID) {
				error("subvolume id %s out of range", arg);
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
static DEFINE_SIMPLE_COMMAND(subvol_sync, "sync");

static const char subvolume_cmd_group_info[] =
"manage subvolumes: create, delete, list, etc";

static const struct cmd_group subvolume_cmd_group = {
	subvolume_cmd_group_usage, subvolume_cmd_group_info, {
		&cmd_struct_subvol_create,
		&cmd_struct_subvol_delete,
		&cmd_struct_subvol_list,
		&cmd_struct_subvol_snapshot,
		&cmd_struct_subvol_get_default,
		&cmd_struct_subvol_set_default,
		&cmd_struct_subvol_find_new,
		&cmd_struct_subvol_show,
		&cmd_struct_subvol_sync,
		NULL
	}
};

DEFINE_GROUP_COMMAND_TOKEN(subvolume);
