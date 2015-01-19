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
#include <libgen.h>
#include <limits.h>
#include <getopt.h>
#include <uuid/uuid.h>

#include "kerncompat.h"
#include "ioctl.h"
#include "qgroup.h"

#include "ctree.h"
#include "commands.h"
#include "utils.h"
#include "btrfs-list.h"
#include "utils.h"

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

	optind = 1;
	while (1) {
		int c = getopt(argc, argv, "c:i:v");
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
	if (res >= 0) {
		fprintf(stderr, "ERROR: '%s' exists\n", dst);
		goto out;
	}

	dupname = strdup(dst);
	newname = basename(dupname);
	dupdir = strdup(dst);
	dstdir = dirname(dupdir);

	if (!test_issubvolname(newname)) {
		fprintf(stderr, "ERROR: incorrect subvolume name '%s'\n",
			newname);
		goto out;
	}

	len = strlen(newname);
	if (len == 0 || len >= BTRFS_VOL_NAME_MAX) {
		fprintf(stderr, "ERROR: subvolume name too long '%s'\n",
			newname);
		goto out;
	}

	fddst = open_file_or_dir(dstdir, &dirstream);
	if (fddst < 0) {
		fprintf(stderr, "ERROR: can't access '%s'\n", dstdir);
		goto out;
	}

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
		fprintf(stderr, "ERROR: cannot create subvolume - %s\n",
			strerror(errno));
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

/*
 * test if path is a subvolume:
 * this function return
 * 0-> path exists but it is not a subvolume
 * 1-> path exists and it is  a subvolume
 * -1 -> path is unaccessible
 */
int test_issubvolume(char *path)
{
	struct stat	st;
	int		res;

	res = stat(path, &st);
	if(res < 0 )
		return -1;

	return (st.st_ino == 256) && S_ISDIR(st.st_mode);
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
	NULL
};

static int cmd_subvol_delete(int argc, char **argv)
{
	int	res, e, ret = 0;
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

	optind = 1;
	while (1) {
		int c;
		static const struct option long_options[] = {
			{"commit-after", no_argument, NULL, 'c'},  /* commit mode 1 */
			{"commit-each", no_argument, NULL, 'C'},  /* commit mode 2 */
			{NULL, 0, NULL, 0}
		};

		c = getopt_long(argc, argv, "cC", long_options, NULL);
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
		fprintf(stderr, "ERROR: error accessing '%s'\n", path);
		ret = 1;
		goto out;
	}
	if (!res) {
		fprintf(stderr, "ERROR: '%s' is not a subvolume\n", path);
		ret = 1;
		goto out;
	}

	cpath = realpath(path, NULL);
	if (!cpath) {
		ret = errno;
		fprintf(stderr, "ERROR: finding real path for '%s': %s\n",
			path, strerror(errno));
		goto out;
	}
	dupdname = strdup(cpath);
	dname = dirname(dupdname);
	dupvname = strdup(cpath);
	vname = basename(dupvname);
	free(cpath);

	fd = open_file_or_dir(dname, &dirstream);
	if (fd < 0) {
		fprintf(stderr, "ERROR: can't access '%s'\n", dname);
		ret = 1;
		goto out;
	}

	printf("Delete subvolume (%s): '%s/%s'\n",
		commit_mode == 2 || (commit_mode == 1 && cnt + 1 == argc)
		? "commit" : "no-commit", dname, vname);
	strncpy_null(args.name, vname);
	res = ioctl(fd, BTRFS_IOC_SNAP_DESTROY, &args);
	e = errno;

	if(res < 0 ){
		fprintf( stderr, "ERROR: cannot delete '%s/%s' - %s\n",
			dname, vname, strerror(e));
		ret = 1;
		goto out;
	}

	if (commit_mode == 1) {
		res = wait_for_commit(fd);
		if (res < 0) {
			fprintf(stderr,
				"ERROR: unable to wait for commit after '%s': %s\n",
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
			fprintf(stderr,
				"ERROR: unable to do final sync: %s\n",
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

	optind = 1;
	while(1) {
		int c;
		static const struct option long_options[] = {
			{"sort", 1, NULL, 'S'},
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
	fd = open_file_or_dir(subvol, &dirstream);
	if (fd < 0) {
		ret = -1;
		fprintf(stderr, "ERROR: can't access '%s'\n", subvol);
		goto out;
	}

	ret = btrfs_list_get_path_rootid(fd, &top_id);
	if (ret) {
		fprintf(stderr, "ERROR: can't get rootid for '%s'\n", subvol);
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
		btrfs_list_free_filter_set(filter_set);
	if (comparer_set)
		btrfs_list_free_comparer_set(comparer_set);
	if (uerr)
		usage(cmd_subvol_list_usage);
	return !!ret;
}

static const char * const cmd_snapshot_usage[] = {
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

static int cmd_snapshot(int argc, char **argv)
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

	optind = 1;
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
			usage(cmd_snapshot_usage);
		}
	}

	if (check_argc_exact(argc - optind, 2))
		usage(cmd_snapshot_usage);

	subvol = argv[optind];
	dst = argv[optind + 1];

	retval = 1;	/* failure */
	res = test_issubvolume(subvol);
	if (res < 0) {
		fprintf(stderr, "ERROR: error accessing '%s'\n", subvol);
		goto out;
	}
	if (!res) {
		fprintf(stderr, "ERROR: '%s' is not a subvolume\n", subvol);
		goto out;
	}

	res = test_isdir(dst);
	if (res == 0) {
		fprintf(stderr, "ERROR: '%s' exists and it is not a directory\n", dst);
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
		fprintf(stderr, "ERROR: incorrect snapshot name '%s'\n",
			newname);
		goto out;
	}

	len = strlen(newname);
	if (len == 0 || len >= BTRFS_VOL_NAME_MAX) {
		fprintf(stderr, "ERROR: snapshot name too long '%s'\n",
			newname);
		goto out;
	}

	fddst = open_file_or_dir(dstdir, &dirstream1);
	if (fddst < 0) {
		fprintf(stderr, "ERROR: can't access '%s'\n", dstdir);
		goto out;
	}

	fd = open_file_or_dir(subvol, &dirstream2);
	if (fd < 0) {
		fprintf(stderr, "ERROR: can't access '%s'\n", dstdir);
		goto out;
	}

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
		fprintf( stderr, "ERROR: cannot snapshot '%s' - %s\n",
			subvol, strerror(errno));
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

	if (check_argc_exact(argc, 2))
		usage(cmd_subvol_get_default_usage);

	subvol = argv[1];
	fd = open_file_or_dir(subvol, &dirstream);
	if (fd < 0) {
		fprintf(stderr, "ERROR: can't access '%s'\n", subvol);
		return 1;
	}

	ret = btrfs_list_get_default_subvolume(fd, &default_id);
	if (ret) {
		fprintf(stderr, "ERROR: can't perform the search - %s\n",
			strerror(errno));
		goto out;
	}

	ret = 1;
	if (default_id == 0) {
		fprintf(stderr, "ERROR: 'default' dir item not found\n");
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
		btrfs_list_free_filter_set(filter_set);
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

	if (check_argc_exact(argc, 3))
		usage(cmd_subvol_set_default_usage);

	subvolid = argv[1];
	path = argv[2];

	objectid = arg_strtou64(subvolid);

	fd = open_file_or_dir(path, &dirstream);
	if (fd < 0) {
		fprintf(stderr, "ERROR: can't access '%s'\n", path);
		return 1;
	}

	ret = ioctl(fd, BTRFS_IOC_DEFAULT_SUBVOL, &objectid);
	e = errno;
	close_file_or_dir(fd, dirstream);
	if (ret < 0) {
		fprintf(stderr, "ERROR: unable to set a new default subvolume - %s\n",
			strerror(e));
		return 1;
	}
	return 0;
}

static const char * const cmd_find_new_usage[] = {
	"btrfs subvolume find-new <path> <lastgen>",
	"List the recently modified files in a filesystem",
	NULL
};

static int cmd_find_new(int argc, char **argv)
{
	int fd;
	int ret;
	char *subvol;
	u64 last_gen;
	DIR *dirstream = NULL;

	if (check_argc_exact(argc, 3))
		usage(cmd_find_new_usage);

	subvol = argv[1];
	last_gen = arg_strtou64(argv[2]);

	ret = test_issubvolume(subvol);
	if (ret < 0) {
		fprintf(stderr, "ERROR: error accessing '%s'\n", subvol);
		return 1;
	}
	if (!ret) {
		fprintf(stderr, "ERROR: '%s' is not a subvolume\n", subvol);
		return 1;
	}

	fd = open_file_or_dir(subvol, &dirstream);
	if (fd < 0) {
		fprintf(stderr, "ERROR: can't access '%s'\n", subvol);
		return 1;
	}

	ret = ioctl(fd, BTRFS_IOC_SYNC);
	if (ret < 0) {
		fprintf(stderr, "ERROR: unable to fs-syncing '%s' - %s\n",
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
	struct btrfs_list_filter_set *filter_set;
	char tstr[256];
	char uuidparse[BTRFS_UUID_UNPARSED_SIZE];
	char *fullpath = NULL, *svpath = NULL, *mnt = NULL;
	char raw_prefix[] = "\t\t\t\t";
	u64 sv_id, mntid;
	int fd = -1, mntfd = -1;
	int ret = 1;
	DIR *dirstream1 = NULL, *dirstream2 = NULL;

	if (check_argc_exact(argc, 2))
		usage(cmd_subvol_show_usage);

	fullpath = realpath(argv[1], NULL);
	if (!fullpath) {
		fprintf(stderr, "ERROR: finding real path for '%s', %s\n",
			argv[1], strerror(errno));
		goto out;
	}

	ret = test_issubvolume(fullpath);
	if (ret < 0) {
		fprintf(stderr, "ERROR: error accessing '%s'\n", fullpath);
		goto out;
	}
	if (!ret) {
		fprintf(stderr, "ERROR: '%s' is not a subvolume\n", fullpath);
		ret = 1;
		goto out;
	}

	ret = find_mount_root(fullpath, &mnt);
	if (ret < 0) {
		fprintf(stderr, "ERROR: find_mount_root failed on '%s': "
				"%s\n", fullpath, strerror(-ret));
		goto out;
	}
	if (ret > 0) {
		fprintf(stderr,
			"ERROR: %s doesn't belong to btrfs mount point\n",
			fullpath);
		goto out;
	}
	ret = 1;
	svpath = get_subvol_name(mnt, fullpath);

	fd = open_file_or_dir(fullpath, &dirstream1);
	if (fd < 0) {
		fprintf(stderr, "ERROR: can't access '%s'\n", fullpath);
		goto out;
	}

	ret = btrfs_list_get_path_rootid(fd, &sv_id);
	if (ret) {
		fprintf(stderr, "ERROR: can't get rootid for '%s'\n",
			fullpath);
		goto out;
	}

	mntfd = open_file_or_dir(mnt, &dirstream2);
	if (mntfd < 0) {
		fprintf(stderr, "ERROR: can't access '%s'\n", mnt);
		goto out;
	}

	ret = btrfs_list_get_path_rootid(mntfd, &mntid);
	if (ret) {
		fprintf(stderr, "ERROR: can't get rootid for '%s'\n", mnt);
		goto out;
	}

	if (sv_id == BTRFS_FS_TREE_OBJECTID) {
		printf("%s is btrfs root\n", fullpath);
		goto out;
	}

	memset(&get_ri, 0, sizeof(get_ri));
	get_ri.root_id = sv_id;

	ret = btrfs_get_subvol(mntfd, &get_ri);
	if (ret) {
		fprintf(stderr, "ERROR: can't find '%s'\n",
			svpath);
		goto out;
	}

	/* print the info */
	printf("%s\n", fullpath);
	printf("\tName: \t\t\t%s\n", get_ri.name);

	if (uuid_is_null(get_ri.uuid))
		strcpy(uuidparse, "-");
	else
		uuid_unparse(get_ri.uuid, uuidparse);
	printf("\tuuid: \t\t\t%s\n", uuidparse);

	if (uuid_is_null(get_ri.puuid))
		strcpy(uuidparse, "-");
	else
		uuid_unparse(get_ri.puuid, uuidparse);
	printf("\tParent uuid: \t\t%s\n", uuidparse);

	if (get_ri.otime) {
		struct tm tm;

		localtime_r(&get_ri.otime, &tm);
		strftime(tstr, 256, "%Y-%m-%d %X", &tm);
	} else
		strcpy(tstr, "-");
	printf("\tCreation time: \t\t%s\n", tstr);

	printf("\tObject ID: \t\t%llu\n", get_ri.root_id);
	printf("\tGeneration (Gen): \t%llu\n", get_ri.gen);
	printf("\tGen at creation: \t%llu\n", get_ri.ogen);
	printf("\tParent: \t\t%llu\n", get_ri.ref_tree);
	printf("\tTop Level: \t\t%llu\n", get_ri.top_id);

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
	btrfs_list_subvols_print(fd, filter_set, NULL, BTRFS_LIST_LAYOUT_RAW,
			1, raw_prefix);

	/* clean up */
	free(get_ri.path);
	free(get_ri.name);
	free(get_ri.full_path);
	btrfs_list_free_filter_set(filter_set);

out:
	close_file_or_dir(fd, dirstream1);
	close_file_or_dir(mntfd, dirstream2);
	free(mnt);
	free(fullpath);
	return !!ret;
}

static const char * const cmd_subvol_sync_usage[] = {
	"btrfs subvolume sync <path> [<subvol-id>...]",
	"Wait until given subvolume(s) are completely removed from the filesystem.",
	"Wait until given subvolume(s) are completely removed from the filesystem",
	"after deletion.",
	"If no subvolume id is given, wait until all ongoing deletion requests",
	"are complete. This may take long if new deleted subvolumes appear during",
	"the sleep interval.",
	"",
	"-s <N>       sleep N seconds between checks (default: 1)",
	NULL
};

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

static int cmd_subvol_sync(int argc, char **argv)
{
	int fd = -1;
	int i;
	int ret = 1;
	DIR *dirstream = NULL;
	u64 *ids = NULL;
	int id_count;
	int remaining;
	int sleep_interval = 1;

	optind = 1;
	while (1) {
		int c = getopt(argc, argv, "s:");

		if (c < 0)
			break;

		switch (c) {
		case 's':
			sleep_interval = atoi(argv[optind]);
			if (sleep_interval < 1) {
				fprintf(stderr,
					"ERROR: invalid sleep interval %s\n",
					argv[optind]);
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

	fd = open_file_or_dir(argv[optind], &dirstream);
	if (fd < 0) {
		fprintf(stderr, "ERROR: can't access '%s'\n", argv[optind]);
		ret = 1;
		goto out;
	}
	optind++;

	id_count = argc - optind;

	/*
	 * Wait for all
	 */
	if (!id_count) {
		while (1) {
			ret = fs_has_dead_subvolumes(fd);
			if (ret < 0) {
				fprintf(stderr, "ERROR: can't perform the search - %s\n",
						strerror(-ret));
				ret = 1;
				goto out;
			}
			if (!ret)
				goto out;
			sleep(sleep_interval);
		}
	}

	/*
	 * Wait only for the requested ones
	 */
	ids = (u64*)malloc(sizeof(u64) * id_count);

	if (!ids) {
		fprintf(stderr, "ERROR: not enough memory\n");
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
			fprintf(stderr, "ERROR: unrecognized subvolume id %s\n",
				arg);
			ret = 1;
			goto out;
		}
		if (id < BTRFS_FIRST_FREE_OBJECTID || id > BTRFS_LAST_FREE_OBJECTID) {
			fprintf(stderr, "ERROR: subvolume id %s out of range\n",
				arg);
			ret = 1;
			goto out;
		}
		ids[i] = id;
	}

	remaining = id_count;
	while (1) {
		for (i = 0; i < id_count; i++) {
			if (!ids[i])
				continue;
			ret = is_subvolume_cleaned(fd, ids[i]);
			if (ret < 0) {
				fprintf(stderr, "ERROR: can't perform the search - %s\n",
						strerror(-ret));
				goto out;
			}
			if (ret) {
				printf("Subvolume id %llu is gone\n", ids[i]);
				ids[i] = 0;
				remaining--;
			}
		}
		if (!remaining)
			break;
		sleep(sleep_interval);
	}

out:
	free(ids);
	close_file_or_dir(fd, dirstream);

	return !!ret;
}

const struct cmd_group subvolume_cmd_group = {
	subvolume_cmd_group_usage, NULL, {
		{ "create", cmd_subvol_create, cmd_subvol_create_usage, NULL, 0 },
		{ "delete", cmd_subvol_delete, cmd_subvol_delete_usage, NULL, 0 },
		{ "list", cmd_subvol_list, cmd_subvol_list_usage, NULL, 0 },
		{ "snapshot", cmd_snapshot, cmd_snapshot_usage, NULL, 0 },
		{ "get-default", cmd_subvol_get_default,
			cmd_subvol_get_default_usage, NULL, 0 },
		{ "set-default", cmd_subvol_set_default,
			cmd_subvol_set_default_usage, NULL, 0 },
		{ "find-new", cmd_find_new, cmd_find_new_usage, NULL, 0 },
		{ "show", cmd_subvol_show, cmd_subvol_show_usage, NULL, 0 },
		{ "sync", cmd_subvol_sync, cmd_subvol_sync_usage, NULL, 0 },
		NULL_CMD_STRUCT
	}
};

int cmd_subvolume(int argc, char **argv)
{
	return handle_command_group(&subvolume_cmd_group, argc, argv);
}
