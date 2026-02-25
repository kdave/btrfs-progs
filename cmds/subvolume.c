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
#include <sys/ioctl.h>
#include <sys/stat.h>
#include <sys/statvfs.h>
#include <inttypes.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <errno.h>
#include <getopt.h>
#include <dirent.h>
#include <stdbool.h>
#include <time.h>
#include <uuid/uuid.h>
#include "libbtrfsutil/btrfsutil.h"
#include "kernel-shared/accessors.h"
#include "kernel-shared/uapi/btrfs_tree.h"
#include "kernel-shared/uapi/btrfs.h"
#include "kernel-shared/ctree.h"
#include "common/defs.h"
#include "common/internal.h"
#include "common/messages.h"
#include "common/utils.h"
#include "common/help.h"
#include "common/path-utils.h"
#include "common/device-scan.h"
#include "common/open-utils.h"
#include "common/string-utils.h"
#include "common/units.h"
#include "common/format-output.h"
#include "common/tree-search.h"
#include "common/parse-utils.h"
#include "cmds/commands.h"
#include "cmds/qgroup.h"

const struct rowspec btrfs_subvolume_rowspec[] = {
	{ .key = "ID", .fmt = "%llu", .out_json = "id" },
	{ .key = "name", .fmt = "str", .out_json = "name" },
	{ .key = "gen", .fmt = "%llu", .out_json = "generation" },
	{ .key = "cgen", .fmt = "%llu", .out_json = "cgeneration" },
	{ .key = "parent", .fmt = "%llu", .out_json = "parent" },
	{ .key = "top level", .fmt = "%llu", .out_json = "top_level" },
	{ .key = "otime", .fmt = "date-time", .out_json = "otime" },
	{ .key = "parent_uuid", .fmt = "uuid", .out_json = "parent_uuid" },
	{ .key = "received_uuid", .fmt = "uuid", .out_json = "received_uuid" },
	{ .key = "uuid", .fmt = "uuid", .out_json = "uuid" },
	{ .key = "path", .fmt = "str", .out_json = "path" },
	{ .key = "flag-list-item", .fmt = "%s" },
	{ .key = "stransid", .fmt = "%llu", .out_json = "stransid" },
	{ .key = "stime", .fmt = "date-time", .out_json = "stime" },
	{ .key = "rtransid", .fmt = "%llu", .out_json = "rtransid" },
	{ .key = "rtime", .fmt = "date-time", .out_json = "rtime" },
	{ .key = "snapshot-list-item", .fmt = "str" },
	{ .key = "quota-qgroup", .fmt = "qgroupid", .out_json = "qgroupid" },
	{ .key = "quota-ref", .fmt = "%llu", .out_json = "referenced" },
	{ .key = "quota-excl", .fmt = "%llu", .out_json = "exclusive" },
	ROWSPEC_END
};

static int wait_for_subvolume_cleaning(int fd, size_t count, uint64_t *ids,
				       int sleep_interval)
{
	size_t i;
	enum btrfs_util_error err;
	size_t done = 0;
	bool statvfs_warned = false;

	pr_verbose(LOG_DEFAULT, "Waiting for %zu subvolume%s\n", count,
			(count > 1 ? "s" : ""));
	while (1) {
		struct statvfs st;
		int ret;
		bool clean = true;

		for (i = 0; i < count; i++) {
			if (!ids[i])
				continue;
			err = btrfs_util_subvolume_get_info_fd(fd, ids[i], NULL);
			if (err == BTRFS_UTIL_ERROR_SUBVOLUME_NOT_FOUND) {
				done++;
				pr_verbose(LOG_DEFAULT, "Subvolume id %" PRIu64 " is gone (%zu/%zu)\n",
				       ids[i], done, count);
				ids[i] = 0;
			} else if (err) {
				error_btrfs_util(err);
				return -errno;
			} else {
				clean = false;
			}
		}
		if (clean)
			break;

		ret = fstatvfs(fd, &st);
		if (ret < 0 && !statvfs_warned) {
			statvfs_warned = true;
			warning("cannot check read-only status of the filesystem: %m");
		} else if (st.f_flag & ST_RDONLY) {
			warning("filesystem is now read-only");
			return 1;
		}
		sleep(sleep_interval);
	}

	return 0;
}

static const char * const subvolume_cmd_group_usage[] = {
	"btrfs subvolume <command> <args>",
	NULL
};

static const char * const cmd_subvolume_create_usage[] = {
	"btrfs subvolume create [options] [<dest>/]<name> [[<dest2>/]<name2> ...]",
	"Create subvolume(s)",
	"Create subvolume(s) at specified destination.  If <dest> is not given",
	"subvolume <name> will be created in the current directory. Options apply",
	"to all created subvolumes.",
	"",
	OPTLINE("-i <qgroupid>", "add the newly created subvolume(s) to a qgroup. This option can be given multiple times."),
	OPTLINE("-p|--parents", "create any missing parent directories for each argument (like mkdir -p)"),
	HELPINFO_INSERT_GLOBALS,
	HELPINFO_INSERT_QUIET,
	NULL
};

static int create_one_subvolume(const char *dst, struct btrfs_util_qgroup_inherit *inherit,
				bool create_parents)
{
	int ret;
	char dupname[PATH_MAX];
	char dupdir[PATH_MAX];
	const char *newname;
	char *dstdir;
	enum btrfs_util_error err;

	strncpy_null(dupname, dst, sizeof(dupname));
	newname = path_basename(dupname);
	strncpy_null(dupdir, dst, sizeof(dupdir));
	dstdir = path_dirname(dupdir);

	if (create_parents) {
		char p[PATH_MAX] = { 0 };
		char dstdir_dup[PATH_MAX];
		char *token;

		strncpy_null(dstdir_dup, dstdir, sizeof(dstdir_dup));
		if (dstdir_dup[0] == '/')
			strcat(p, "/");

		token = strtok(dstdir_dup, "/");
		while (token) {
			strcat(p, token);
			ret = path_is_dir(p);
			if (ret == -ENOENT) {
				ret = mkdir(p, 0777);
				if (ret < 0) {
					error("failed to create directory %s: %m", p);
					goto out;
				}
			} else if (ret <= 0) {
				if (ret == 0)
					ret = -EEXIST;
				errno = ret ;
				error("failed to check directory %s before creation: %m", p);
				goto out;
			}
			strcat(p, "/");
			token = strtok(NULL, "/");
		}
	}

	err = btrfs_util_subvolume_create(dst, 0, NULL, inherit);
	if (err) {
		error_btrfs_util(err);
		ret = -errno;
		goto out;
	}

	pr_verbose(LOG_DEFAULT, "Create subvolume '%s/%s'\n", dstdir, newname);

	ret = 0;

out:

	return ret;
}

static int qgroup_inherit_add_group(struct btrfs_util_qgroup_inherit **inherit,
				    const char *arg)
{
	enum btrfs_util_error err;
	u64 qgroupid;

	if (!*inherit) {
		err = btrfs_util_qgroup_inherit_create(0, inherit);
		if (err) {
			error_btrfs_util(err);
			return -errno;
		}
	}

	qgroupid = parse_qgroupid_or_path(optarg);
	if (qgroupid == 0) {
		error("invalid qgroup specification, qgroupid must not be 0");
		return -EINVAL;
	}

	err = btrfs_util_qgroup_inherit_add_group(inherit, qgroupid);
	if (err) {
		error_btrfs_util(err);
		return -errno;
	}

	return 0;
}

static int cmd_subvolume_create(const struct cmd_struct *cmd, int argc, char **argv)
{
	int retval, ret;
	struct btrfs_util_qgroup_inherit *inherit = NULL;
	bool has_error = false;
	bool create_parents = false;

	optind = 0;
	while (1) {
		int c;
		static const struct option long_options[] = {
			{ "parents", no_argument, NULL, 'p' },
			{ NULL, 0, NULL, 0 }
		};

		c = getopt_long(argc, argv, "i:p", long_options, NULL);
		if (c < 0)
			break;

		switch (c) {
		case 'i':
			ret = qgroup_inherit_add_group(&inherit, optarg);
			if (ret) {
				retval = ret;
				goto out;
			}
			break;
		case 'p':
			create_parents = true;
			break;
		default:
			usage_unknown_option(cmd, argv);
		}
	}

	if (check_argc_min(argc - optind, 1)) {
		retval = 1;
		goto out;
	}

	retval = 1;

	for (int i = optind; i < argc; i++) {
		ret = create_one_subvolume(argv[i], inherit, create_parents);
		if (ret < 0)
			has_error = true;
	}
	if (!has_error)
		retval = 0;
out:
	btrfs_util_qgroup_inherit_destroy(inherit);

	return retval;
}
static DEFINE_SIMPLE_COMMAND(subvolume_create, "create");

static int wait_for_commit(int fd)
{
	enum btrfs_util_error err;
	uint64_t transid;

	err = btrfs_util_fs_start_sync_fd(fd, &transid);
	if (err)
		return -1;

	err = btrfs_util_fs_wait_sync_fd(fd, transid);
	if (err)
		return -1;

	return 0;
}

static const char * const cmd_subvolume_delete_usage[] = {
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
	OPTLINE("-c|--commit-after", "wait for transaction commit at the end of the operation"),
	OPTLINE("-C|--commit-each", "wait for transaction commit after deleting each subvolume"),
	OPTLINE("-i|--subvolid", "subvolume id of the to be removed subvolume"),
	OPTLINE("-R|--recursive", "delete accessible subvolumes beneath each subvolume recursively, "
		"this is not atomic, may need root to delete subvolumes not accessible by the user"),
	OPTLINE("-v|--verbose", "deprecated, alias for global -v option"),
	HELPINFO_INSERT_GLOBALS,
	HELPINFO_INSERT_VERBOSE,
	HELPINFO_INSERT_QUIET,
	HELPINFO_INSERT_DRY_RUN,
	NULL
};

static int cmd_subvolume_delete(const struct cmd_struct *cmd, int argc, char **argv)
{
	int res, ret = 0;
	int cnt;
	int fd = -1;
	char	*dname, *cpath;
	const char *vname;
	char	*dupdname = NULL;
	char	*dupvname = NULL;
	char	*path = NULL;
	int commit_mode = 0;
	bool subvol_path_not_found = false;
	int flags = 0;
	u8 fsid[BTRFS_FSID_SIZE];
	u64 subvolid = 0;
	char uuidbuf[BTRFS_UUID_UNPARSED_SIZE];
	char full_subvolpath[BTRFS_SUBVOL_NAME_MAX];
	struct seen_fsid *seen_fsid_hash[SEEN_FSID_HASH_SIZE] = { NULL, };
	enum { COMMIT_AFTER = 1, COMMIT_EACH = 2 };
	enum btrfs_util_error err;
	uint64_t default_subvol_id, target_subvol_id = 0;

	optind = 0;
	while (1) {
		int c;
		static const struct option long_options[] = {
			{"commit-after", no_argument, NULL, 'c'},
			{"commit-each", no_argument, NULL, 'C'},
			{"subvolid", required_argument, NULL, 'i'},
			{"recursive", no_argument, NULL, 'R'},
			{"verbose", no_argument, NULL, 'v'},
			{NULL, 0, NULL, 0}
		};

		c = getopt_long(argc, argv, "cCi:Rv", long_options, NULL);
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
		case 'R':
			flags |= BTRFS_UTIL_DELETE_SUBVOLUME_RECURSIVE;
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

	if (subvolid > 0 && flags & BTRFS_UTIL_DELETE_SUBVOLUME_RECURSIVE) {
		error("option --recursive is not supported with --subvolid");
		return 1;
	}

	pr_verbose(LOG_INFO, "Transaction commit: %s\n",
		   !commit_mode ? "none (default)" :
		   commit_mode == COMMIT_AFTER ? "at the end" : "after each");

	cnt = optind;

	/* Check the following syntax: subvolume delete --subvolid <subvolid> <path> */
	if (subvolid > 0) {
		char *subvol;

		path = argv[cnt];
		err = btrfs_util_subvolume_get_path(path, subvolid, &subvol);
		/*
		 * If the subvolume is really not referred by anyone, and refs
		 * is 0, newer kernel can handle it by just adding an orphan
		 * item and queue it for cleanup.
		 *
		 * In this case, just let kernel to handle it, we do no extra
		 * handling.
		 */
		if (err == BTRFS_UTIL_ERROR_SUBVOLUME_NOT_FOUND) {
			subvol_path_not_found = true;
			goto again;
		}
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

	err = btrfs_util_subvolume_is_valid(path);
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
	dname = path_dirname(dupdname);
	dupvname = strdup(cpath);
	vname = path_basename(dupvname);
	free(cpath);

	/* When subvolid is passed, <path> will point to the mount point */
	if (subvolid > 0)
		dname = dupvname;

	fd = btrfs_open_dir(dname);
	if (fd < 0) {
		ret = 1;
		goto out;
	}

	default_subvol_id = 0;
	err = btrfs_util_subvolume_get_default_fd(fd, &default_subvol_id);
	if (err == BTRFS_UTIL_ERROR_SEARCH_FAILED) {
		if (geteuid() == 0)
			warning("cannot read default subvolume id: %m");
	}

	if (subvolid > 0) {
		target_subvol_id = subvolid;
	} else {
		err = btrfs_util_subvolume_get_id(path, &target_subvol_id);
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

	if (flags & BTRFS_UTIL_DELETE_SUBVOLUME_RECURSIVE) {
		struct btrfs_util_subvolume_iterator *iter;

		err = btrfs_util_subvolume_iter_create_fd(fd, target_subvol_id,
				  BTRFS_UTIL_SUBVOLUME_ITERATOR_POST_ORDER,
				  &iter);
		if (!err) {
			char *nested_path;
			struct btrfs_util_subvolume_info subvol_info;

			while (!(err = btrfs_util_subvolume_iter_next_info(iter, &nested_path, &subvol_info))) {
				pr_verbose(LOG_DEFAULT, "Delete subvolume %" PRIu64 " (%s): ",
					   subvol_info.id,
					   commit_mode == COMMIT_EACH ||
					   (commit_mode == COMMIT_AFTER && cnt + 1 == argc) ?
					   "commit" : "no-commit");
				pr_verbose(LOG_DEFAULT, "'%s/%s/%s'\n", dname, vname, nested_path);

				free(nested_path);
			}
			if (err != BTRFS_UTIL_ERROR_STOP_ITERATION)
				warning("failed to iterate subvolumes, nested subvolumes will not be printed: %s", btrfs_util_strerror(err));

			btrfs_util_subvolume_iter_destroy(iter);
		} else {
			warning("failed to create subvolume iterator, nested subvolumes will not be printed: %s", btrfs_util_strerror(err));
		}
	}

	pr_verbose(LOG_DEFAULT, "Delete subvolume %" PRIu64 " (%s): ",
		target_subvol_id,
		commit_mode == COMMIT_EACH ||
		(commit_mode == COMMIT_AFTER && cnt + 1 == argc) ?
		"commit" : "no-commit");

	if (subvolid == 0)
		pr_verbose(LOG_DEFAULT, "'%s/%s'\n", dname, vname);
	else if (!subvol_path_not_found)
		pr_verbose(LOG_DEFAULT, "'%s'\n", full_subvolpath);
	else
		pr_verbose(LOG_DEFAULT, "subvolid=%llu\n", subvolid);

	if (bconf_is_dry_run())
		goto out;

	/* Start deleting. */
	if (subvolid == 0)
		err = btrfs_util_subvolume_delete_fd(fd, vname, flags);
	else
		err = btrfs_util_subvolume_delete_by_id_fd(fd, subvolid);
	if (err) {
		int saved_errno = errno;

		error_btrfs_util(err);
		if (saved_errno == EPERM)
			warning("deletion failed with EPERM, you don't have permissions or send may be in progress or the subvolume is set as default");
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

		if (add_seen_fsid(fsid, seen_fsid_hash, fd) == 0) {
			uuid_unparse(fsid, uuidbuf);
			pr_verbose(LOG_INFO, "  new fs is found for '%s', fsid: %s\n",
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
	close(fd);
keep_fd:
	fd = -1;
	free(dupdname);
	free(dupvname);
	dupdname = NULL;
	dupvname = NULL;
	cnt++;
	if (cnt < argc)
		goto again;

	if (commit_mode == COMMIT_AFTER && !bconf_is_dry_run()) {
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
					pr_verbose(LOG_INFO,
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
static DEFINE_COMMAND_WITH_FLAGS(subvolume_delete, "delete", CMD_DRY_RUN);

static const char * const cmd_subvolume_snapshot_usage[] = {
	"btrfs subvolume snapshot [-r] [-i <qgroupid>] <subvolume> { <subdir>/<name> | <subdir> }",
	"",
	"Create a snapshot of a <subvolume>. Call it <name> and place it in the <subdir>.",
	"(<subvolume> will look like a new sub-directory, but is actually a btrfs subvolume",
	"not a sub-directory.)",
	"",
	"When only <subdir> is given, the subvolume will be named the basename of <subvolume>.",
	"",
	OPTLINE("-r", "make the new snapshot readonly"),
	OPTLINE("-i <qgroupid>", "Add the new snapshot to a qgroup (a quota group). This option can be given multiple times."),
	HELPINFO_INSERT_GLOBALS,
	HELPINFO_INSERT_QUIET,
	NULL
};

static int cmd_subvolume_snapshot(const struct cmd_struct *cmd, int argc, char **argv)
{
	char	*subvol, *dst;
	int	res, retval;
	char	*dstdir = NULL;
	enum btrfs_util_error err;
	struct btrfs_util_qgroup_inherit *inherit = NULL;
	int flags = 0;

	optind = 0;
	while (1) {
		int c = getopt(argc, argv, "i:r");
		if (c < 0)
			break;

		switch (c) {
		case 'i':
			res = qgroup_inherit_add_group(&inherit, optarg);
			if (res) {
				retval = res;
				goto out;
			}
			break;
		case 'r':
			flags |= BTRFS_UTIL_CREATE_SNAPSHOT_READ_ONLY;
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
	err = btrfs_util_subvolume_is_valid(subvol);
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
		char *dupname;
		const char *newname;

		dupname = strdup(subvol);
		newname = path_basename(dupname);

		dstdir = malloc(strlen(dst) + 1 + strlen(newname) + 1);
		if (!dstdir) {
			error_mem(NULL);
			free(dupname);
			goto out;
		}

		dstdir[0] = 0;
		strcpy(dstdir, dst);
		strcat(dstdir, "/");
		strcat(dstdir, newname);

		free(dupname);
	} else {
		dstdir = strdup(dst);
	}

	err = btrfs_util_subvolume_snapshot(subvol, dstdir, flags, NULL, inherit);
	if (err) {
		error_btrfs_util(err);
		goto out;
	}

	retval = 0;	/* success */

	if (flags & BTRFS_UTIL_CREATE_SNAPSHOT_READ_ONLY)
		pr_verbose(LOG_DEFAULT,
			   "Create readonly snapshot of '%s' in '%s'\n",
			   subvol, dstdir);
	else
		pr_verbose(LOG_DEFAULT,
			   "Create snapshot of '%s' in '%s'\n",
			   subvol, dstdir);

out:
	free(dstdir);
	btrfs_util_qgroup_inherit_destroy(inherit);

	return retval;
}
static DEFINE_SIMPLE_COMMAND(subvolume_snapshot, "snapshot");

static const char * const cmd_subvolume_get_default_usage[] = {
	"btrfs subvolume get-default <path>",
	"Get the default subvolume of a filesystem",
#if EXPERIMENTAL
	HELPINFO_INSERT_GLOBALS,
	HELPINFO_INSERT_FORMAT,
#endif
	NULL
};

static int cmd_subvolume_get_default(const struct cmd_struct *cmd, int argc, char **argv)
{
	int fd = -1;
	int ret = 1;
	uint64_t default_id;
	enum btrfs_util_error err;
	struct btrfs_util_subvolume_info subvol;
	struct format_ctx fctx;
	char *path;

	clean_args_no_options(cmd, argc, argv);

	if (check_argc_exact(argc - optind, 1))
		return 1;

	fd = btrfs_open_dir(argv[1]);
	if (fd < 0)
		return 1;

	err = btrfs_util_subvolume_get_default_fd(fd, &default_id);
	if (err) {
		error_btrfs_util(err);
		goto out;
	}

	/* no need to resolve roots if FS_TREE is default */
	if (default_id == BTRFS_FS_TREE_OBJECTID) {
		if (bconf.output_format == CMD_FORMAT_JSON) {
			fmt_start(&fctx, btrfs_subvolume_rowspec, 1, 0);
			fmt_print(&fctx, "ID", 5);
			fmt_end(&fctx);
		} else {
			pr_verbose(LOG_DEFAULT, "ID 5 (FS_TREE)\n");
		}

		ret = 0;
		goto out;
	}

	err = btrfs_util_subvolume_get_info_fd(fd, default_id, &subvol);
	if (err) {
		error_btrfs_util(err);
		goto out;
	}

	err = btrfs_util_subvolume_get_path_fd(fd, default_id, &path);
	if (err) {
		error_btrfs_util(err);
		goto out;
	}

	if (bconf.output_format == CMD_FORMAT_JSON) {
		fmt_start(&fctx, btrfs_subvolume_rowspec, 1, 0);
		fmt_print(&fctx, "ID", subvol.id);
		fmt_print(&fctx, "gen", subvol.generation);
		fmt_print(&fctx, "top level", subvol.parent_id);
		fmt_print(&fctx, "path", path);
		fmt_end(&fctx);
	} else {
		pr_verbose(LOG_DEFAULT, "ID %" PRIu64 " gen %" PRIu64 " top level %" PRIu64 " path %s\n",
		       subvol.id, subvol.generation, subvol.parent_id, path);
	}

	free(path);

	ret = 0;
out:
	close(fd);
	return ret;
}
#if EXPERIMENTAL
static DEFINE_COMMAND_WITH_FLAGS(subvolume_get_default, "get-default", CMD_FORMAT_JSON);
#else
DEFINE_SIMPLE_COMMAND(subvolume_get_default, "get-default");
#endif

static const char * const cmd_subvolume_set_default_usage[] = {
	"btrfs subvolume set-default <subvolume>\n"
	"btrfs subvolume set-default <subvolid> <path>",
	"Set the default subvolume of the filesystem mounted as default.",
	"The subvolume can be specified by its path,",
	"or the pair of subvolume id and path to the filesystem.",
	NULL
};

static int cmd_subvolume_set_default(const struct cmd_struct *cmd, int argc, char **argv)
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

	err = btrfs_util_subvolume_set_default(path, objectid);
	if (err) {
		error_btrfs_util(err);
		return 1;
	}
	return 0;
}
static DEFINE_SIMPLE_COMMAND(subvolume_set_default, "set-default");

static const char * const cmd_subvolume_find_new_usage[] = {
	"btrfs subvolume find-new <path> <lastgen>",
	"List the recently modified files in a filesystem",
	NULL
};

/* finding the generation for a given path is a two step process.
 * First we use the inode lookup routine to find out the root id
 *
 * Then we use the tree search ioctl to scan all the root items for a
 * given root id and spit out the latest generation we can find
 */
static u64 find_root_gen(int fd)
{
	struct btrfs_ioctl_ino_lookup_args ino_args;
	int ret;
	struct btrfs_tree_search_args args;
	struct btrfs_ioctl_search_key *sk;
	unsigned long off = 0;
	u64 max_found = 0;
	int i;

	memset(&ino_args, 0, sizeof(ino_args));
	ino_args.objectid = BTRFS_FIRST_FREE_OBJECTID;

	/* this ioctl fills in ino_args->treeid */
	ret = ioctl(fd, BTRFS_IOC_INO_LOOKUP, &ino_args);
	if (ret < 0) {
		error("failed to lookup path for dirid %llu: %m", BTRFS_FIRST_FREE_OBJECTID);
		return 0;
	}

	memset(&args, 0, sizeof(args));
	sk = btrfs_tree_search_sk(&args);
	sk->tree_id = BTRFS_ROOT_TREE_OBJECTID;

	/*
	 * there may be more than one ROOT_ITEM key if there are
	 * snapshots pending deletion, we have to loop through
	 * them.
	 */
	sk->min_objectid = ino_args.treeid;
	sk->min_type = BTRFS_ROOT_ITEM_KEY;
	sk->max_objectid = ino_args.treeid;
	sk->max_type = BTRFS_ROOT_ITEM_KEY;
	sk->max_offset = (u64)-1;
	sk->max_transid = (u64)-1;
	sk->nr_items = 4096;

	while (1) {
		ret = btrfs_tree_search_ioctl(fd, &args);
		if (ret < 0) {
			error("can't perform the search: %m");
			return 0;
		}
		/* the ioctl returns the number of item it found in nr_items */
		if (sk->nr_items == 0)
			break;

		off = 0;
		for (i = 0; i < sk->nr_items; i++) {
			struct btrfs_root_item *item;
			struct btrfs_ioctl_search_header sh;

			memcpy(&sh, btrfs_tree_search_data(&args, off), sizeof(sh));
			off += sizeof(sh);
			item = btrfs_tree_search_data(&args, off);
			off += sh.len;

			sk->min_objectid = sh.objectid;
			sk->min_type = sh.type;
			sk->min_offset = sh.offset;

			if (sh.objectid > ino_args.treeid)
				break;

			if (sh.objectid == ino_args.treeid &&
			    sh.type == BTRFS_ROOT_ITEM_KEY) {
				max_found = max(max_found,
						btrfs_root_generation(item));
			}
		}
		if (sk->min_offset < (u64)-1)
			sk->min_offset++;
		else
			break;

		if (sk->min_type != BTRFS_ROOT_ITEM_KEY)
			break;
		if (sk->min_objectid != ino_args.treeid)
			break;
	}
	return max_found;
}

/* pass in a directory id and this will return
 * the full path of the parent directory inside its
 * subvolume root.
 *
 * It may return NULL if it is in the root, or an ERR_PTR if things
 * go badly.
 */
static char *__ino_resolve(int fd, u64 dirid)
{
	struct btrfs_ioctl_ino_lookup_args args;
	int ret;
	char *full;

	memset(&args, 0, sizeof(args));
	args.objectid = dirid;

	ret = ioctl(fd, BTRFS_IOC_INO_LOOKUP, &args);
	if (ret < 0) {
		error("failed to lookup path for dirid %llu: %m", dirid);
		return ERR_PTR(ret);
	}

	if (args.name[0]) {
		/*
		 * we're in a subdirectory of ref_tree, the kernel ioctl
		 * puts a / in there for us
		 */
		full = strdup(args.name);
		if (!full) {
			error_mem(NULL);
			return ERR_PTR(-ENOMEM);
		}
	} else {
		/* we're at the root of ref_tree */
		full = NULL;
	}
	return full;
}

/*
 * simple string builder, returning a new string with both
 * dirid and name
 */
static char *build_name(const char *dirid, const char *name)
{
	char *full;

	if (!dirid)
		return strdup(name);

	full = malloc(strlen(dirid) + strlen(name) + 1);
	if (!full)
		return NULL;
	strcpy(full, dirid);
	strcat(full, name);
	return full;
}

/*
 * given an inode number, this returns the full path name inside the subvolume
 * to that file/directory.  cache_dirid and cache_name are used to
 * cache the results so we can avoid tree searches if a later call goes
 * to the same directory or file name
 */
static char *ino_resolve(int fd, u64 ino, u64 *cache_dirid, char **cache_name)

{
	u64 dirid;
	char *dirname;
	char *name;
	char *full;
	int ret;
	struct btrfs_tree_search_args args;
	struct btrfs_ioctl_search_key *sk;
	struct btrfs_ioctl_search_header sh;
	int namelen;

	memset(&args, 0, sizeof(args));
	sk = btrfs_tree_search_sk(&args);
	sk->tree_id = 0;

	/*
	 * step one, we search for the inode back ref.  We just use the first
	 * one
	 */
	sk->min_objectid = ino;
	sk->max_objectid = ino;
	sk->max_type = BTRFS_INODE_REF_KEY;
	sk->max_offset = (u64)-1;
	sk->min_type = BTRFS_INODE_REF_KEY;
	sk->max_transid = (u64)-1;
	sk->nr_items = 1;

	ret = btrfs_tree_search_ioctl(fd, &args);
	if (ret < 0) {
		error("can't perform the search: %m");
		return NULL;
	}
	/* the ioctl returns the number of item it found in nr_items */
	if (sk->nr_items == 0)
		return NULL;

	memcpy(&sh, btrfs_tree_search_data(&args, 0), sizeof(sh));
	if (sh.type == BTRFS_INODE_REF_KEY) {
		struct btrfs_inode_ref *ref;

		dirid = sh.offset;

		ref = btrfs_tree_search_data(&args, sizeof(sh));
		namelen = btrfs_stack_inode_ref_name_len(ref);

		name = (char *)(ref + 1);
		name = strndup(name, namelen);
		if (!name) {
			error_mem(NULL);
			return NULL;
		}

		/* use our cached value */
		if (dirid == *cache_dirid && *cache_name) {
			dirname = *cache_name;
			goto build;
		}
	} else {
		return NULL;
	}
	/*
	 * the inode backref gives us the file name and the parent directory id.
	 * From here we use __ino_resolve to get the path to the parent
	 */
	dirname = __ino_resolve(fd, dirid);
build:
	full = build_name(dirname, name);
	if (*cache_name && dirname != *cache_name)
		free(*cache_name);

	*cache_name = dirname;
	*cache_dirid = dirid;
	free(name);

	return full;
}

static int print_one_extent(int fd, struct btrfs_ioctl_search_header *sh,
			    struct btrfs_file_extent_item *item,
			    u64 found_gen, u64 *cache_dirid,
			    char **cache_dir_name, u64 *cache_ino,
			    char **cache_full_name)
{
	u64 len = 0;
	u64 disk_start = 0;
	u64 disk_offset = 0;
	u8 type;
	int compressed = 0;
	int flags = 0;
	char *name = NULL;

	if (btrfs_search_header_objectid(sh) == *cache_ino) {
		name = *cache_full_name;
	} else if (*cache_full_name) {
		free(*cache_full_name);
		*cache_full_name = NULL;
	}
	if (!name) {
		name = ino_resolve(fd, btrfs_search_header_objectid(sh),
				   cache_dirid,
				   cache_dir_name);
		*cache_full_name = name;
		*cache_ino = btrfs_search_header_objectid(sh);
	}
	if (!name)
		return -EIO;

	type = btrfs_stack_file_extent_type(item);
	compressed = btrfs_stack_file_extent_compression(item);

	if (type == BTRFS_FILE_EXTENT_REG ||
	    type == BTRFS_FILE_EXTENT_PREALLOC) {
		disk_start = btrfs_stack_file_extent_disk_bytenr(item);
		disk_offset = btrfs_stack_file_extent_offset(item);
		len = btrfs_stack_file_extent_num_bytes(item);
	} else if (type == BTRFS_FILE_EXTENT_INLINE) {
		disk_start = 0;
		disk_offset = 0;
		len = btrfs_stack_file_extent_ram_bytes(item);
	} else {
		error(
	"unhandled extent type %d for inode %llu file offset %llu gen %llu",
			type,
			btrfs_search_header_objectid(sh),
			btrfs_search_header_offset(sh),
			found_gen);

		return -EIO;
	}
	pr_verbose(LOG_DEFAULT, "inode %llu file offset %llu len %llu disk start %llu "
	       "offset %llu gen %llu flags ",
	       btrfs_search_header_objectid(sh), btrfs_search_header_offset(sh),
	       len, disk_start, disk_offset, found_gen);

	if (compressed) {
		pr_verbose(LOG_DEFAULT, "COMPRESS");
		flags++;
	}
	if (type == BTRFS_FILE_EXTENT_PREALLOC) {
		pr_verbose(LOG_DEFAULT, "%sPREALLOC", flags ? "|" : "");
		flags++;
	}
	if (type == BTRFS_FILE_EXTENT_INLINE) {
		pr_verbose(LOG_DEFAULT, "%sINLINE", flags ? "|" : "");
		flags++;
	}
	if (!flags)
		pr_verbose(LOG_DEFAULT, "NONE");

	pr_verbose(LOG_DEFAULT, " %s\n", name);
	return 0;
}

static int btrfs_list_find_updated_files(int fd, u64 root_id, u64 oldest_gen)
{
	int ret;
	struct btrfs_tree_search_args args;
	struct btrfs_ioctl_search_key *sk;
	struct btrfs_file_extent_item *item;
	unsigned long off = 0;
	u64 found_gen;
	u64 max_found = 0;
	int i;
	u64 cache_dirid = 0;
	u64 cache_ino = 0;
	char *cache_dir_name = NULL;
	char *cache_full_name = NULL;
	struct btrfs_file_extent_item backup;

	memset(&backup, 0, sizeof(backup));
	memset(&args, 0, sizeof(args));
	sk = btrfs_tree_search_sk(&args);
	sk->tree_id = root_id;

	/*
	 * set all the other params to the max, we'll take any objectid
	 * and any trans
	 */
	sk->max_objectid = (u64)-1;
	sk->max_offset = (u64)-1;
	sk->max_transid = (u64)-1;
	sk->max_type = BTRFS_EXTENT_DATA_KEY;
	sk->min_transid = oldest_gen;
	/* just a big number, doesn't matter much */
	sk->nr_items = 4096;

	max_found = find_root_gen(fd);
	while(1) {
		ret = btrfs_tree_search_ioctl(fd, &args);
		if (ret < 0) {
			error("can't perform the search: %m");
			break;
		}
		/* the ioctl returns the number of item it found in nr_items */
		if (sk->nr_items == 0)
			break;

		off = 0;

		/*
		 * for each item, pull the key out of the header and then
		 * read the root_ref item it contains
		 */
		for (i = 0; i < sk->nr_items; i++) {
			struct btrfs_ioctl_search_header sh;

			memcpy(&sh, btrfs_tree_search_data(&args, off), sizeof(sh));
			off += sizeof(sh);

			/*
			 * just in case the item was too big, pass something other
			 * than garbage
			 */
			if (sh.len == 0)
				item = &backup;
			else
				item = btrfs_tree_search_data(&args, off);

			found_gen = btrfs_stack_file_extent_generation(item);
			if (sh.type == BTRFS_EXTENT_DATA_KEY &&
			    found_gen >= oldest_gen) {
				print_one_extent(fd, &sh, item, found_gen,
						 &cache_dirid, &cache_dir_name,
						 &cache_ino, &cache_full_name);
			}
			off += sh.len;

			/*
			 * record the mins in sk so we can make sure the
			 * next search doesn't repeat this root
			 */
			sk->min_objectid = sh.objectid;
			sk->min_type = sh.type;
			sk->min_offset = sh.offset;
		}
		sk->nr_items = 4096;
		if (sk->min_offset < (u64)-1)
			sk->min_offset++;
		else if (sk->min_objectid < (u64)-1) {
			sk->min_objectid++;
			sk->min_offset = 0;
			sk->min_type = 0;
		} else
			break;
	}
	free(cache_dir_name);
	free(cache_full_name);
	pr_verbose(LOG_DEFAULT, "transid marker was %llu\n", max_found);
	return ret;
}

static int cmd_subvolume_find_new(const struct cmd_struct *cmd, int argc, char **argv)
{
	int fd;
	int ret;
	char *subvol;
	u64 last_gen;
	enum btrfs_util_error err;

	clean_args_no_options(cmd, argc, argv);

	if (check_argc_exact(argc - optind, 2))
		return 1;

	subvol = argv[optind];
	last_gen = arg_strtou64(argv[optind + 1]);

	err = btrfs_util_subvolume_is_valid(subvol);
	if (err) {
		error_btrfs_util(err);
		return 1;
	}

	fd = btrfs_open_dir(subvol);
	if (fd < 0)
		return 1;

	err = btrfs_util_fs_sync_fd(fd);
	if (err) {
		error_btrfs_util(err);
		close(fd);
		return 1;
	}

	ret = btrfs_list_find_updated_files(fd, 0, last_gen);
	close(fd);
	return !!ret;
}
static DEFINE_SIMPLE_COMMAND(subvolume_find_new, "find-new");

static void print_subvolume_show_text(const struct btrfs_util_subvolume_info *subvol,
				      const char *subvol_path, const char *subvol_name)
{
	char tstr[256];
	char uuidparse[BTRFS_UUID_UNPARSED_SIZE];

	/* Warn if it's a read-write subvolume with received_uuid */
	if (!uuid_is_null(subvol->received_uuid) &&
	    !(subvol->flags & BTRFS_ROOT_SUBVOL_RDONLY)) {
		warning("the subvolume is read-write and has received_uuid set,\n"
			"\t don't use it for incremental send. Please see section\n"
			"\t 'SUBVOLUME FLAGS' in manual page btrfs-subvolume for\n"
			"\t further information.");
	}

	/* print the info */
	pr_verbose(LOG_DEFAULT, "%s\n",
		   subvol->id == BTRFS_FS_TREE_OBJECTID ? "/" : subvol_path);
	pr_verbose(LOG_DEFAULT, "\tName: \t\t\t%s\n", subvol_name);

	if (uuid_is_null(subvol->uuid))
		strcpy(uuidparse, "-");
	else
		uuid_unparse(subvol->uuid, uuidparse);
	pr_verbose(LOG_DEFAULT, "\tUUID: \t\t\t%s\n", uuidparse);

	if (uuid_is_null(subvol->parent_uuid))
		strcpy(uuidparse, "-");
	else
		uuid_unparse(subvol->parent_uuid, uuidparse);
	pr_verbose(LOG_DEFAULT, "\tParent UUID: \t\t%s\n", uuidparse);

	if (uuid_is_null(subvol->received_uuid))
		strcpy(uuidparse, "-");
	else
		uuid_unparse(subvol->received_uuid, uuidparse);
	pr_verbose(LOG_DEFAULT, "\tReceived UUID: \t\t%s\n", uuidparse);

	if (subvol->otime.tv_sec) {
		struct tm tm;

		localtime_r(&subvol->otime.tv_sec, &tm);
		strftime(tstr, 256, "%Y-%m-%d %X %z", &tm);
	} else
		strcpy(tstr, "-");
	pr_verbose(LOG_DEFAULT, "\tCreation time: \t\t%s\n", tstr);

	pr_verbose(LOG_DEFAULT, "\tSubvolume ID: \t\t%" PRIu64 "\n", subvol->id);
	pr_verbose(LOG_DEFAULT, "\tGeneration: \t\t%" PRIu64 "\n", subvol->generation);
	pr_verbose(LOG_DEFAULT, "\tGen at creation: \t%" PRIu64 "\n", subvol->otransid);
	pr_verbose(LOG_DEFAULT, "\tParent ID: \t\t%" PRIu64 "\n", subvol->parent_id);
	pr_verbose(LOG_DEFAULT, "\tTop level ID: \t\t%" PRIu64 "\n", subvol->parent_id);

	if (subvol->flags & BTRFS_ROOT_SUBVOL_RDONLY)
		pr_verbose(LOG_DEFAULT, "\tFlags: \t\t\treadonly\n");
	else
		pr_verbose(LOG_DEFAULT, "\tFlags: \t\t\t-\n");

	pr_verbose(LOG_DEFAULT, "\tSend transid: \t\t%" PRIu64 "\n", subvol->stransid);
	pr_verbose(LOG_DEFAULT, "\tSend time: \t\t%s\n", tstr);
	if (subvol->stime.tv_sec) {
		struct tm tm;

		localtime_r(&subvol->stime.tv_sec, &tm);
		strftime(tstr, 256, "%Y-%m-%d %X %z", &tm);
	} else {
		strcpy(tstr, "-");
	}
	pr_verbose(LOG_DEFAULT, "\tReceive transid: \t%" PRIu64 "\n", subvol->rtransid);
	if (subvol->rtime.tv_sec) {
		struct tm tm;

		localtime_r(&subvol->rtime.tv_sec, &tm);
		strftime(tstr, 256, "%Y-%m-%d %X %z", &tm);
	} else {
		strcpy(tstr, "-");
	}
	pr_verbose(LOG_DEFAULT, "\tReceive time: \t\t%s\n", tstr);
}

static void print_subvolume_show_quota_text(const struct btrfs_util_subvolume_info *subvol,
					    const struct btrfs_qgroup_stats *stats,
					    unsigned int unit_mode)
{
	pr_verbose(LOG_DEFAULT, "\tQuota group:\t\t0/%" PRIu64 "\n", subvol->id);
	fflush(stdout);

	pr_verbose(LOG_DEFAULT, "\t  Limit referenced:\t%s\n",
			stats->limit.max_referenced == 0 ? "-" :
			pretty_size_mode(stats->limit.max_referenced, unit_mode));
	pr_verbose(LOG_DEFAULT, "\t  Limit exclusive:\t%s\n",
			stats->limit.max_exclusive == 0 ? "-" :
			pretty_size_mode(stats->limit.max_exclusive, unit_mode));
	pr_verbose(LOG_DEFAULT, "\t  Usage referenced:\t%s\n",
			pretty_size_mode(stats->info.referenced, unit_mode));
	pr_verbose(LOG_DEFAULT, "\t  Usage exclusive:\t%s\n",
			pretty_size_mode(stats->info.exclusive, unit_mode));
}

static void print_subvolume_show_json(struct format_ctx *fctx,
				      const struct btrfs_util_subvolume_info *subvol,
				      const char *subvol_path, const char *subvol_name)
{
	fmt_print(fctx, "name", subvol_name);

	fmt_print(fctx, "uuid", subvol->uuid);
	fmt_print(fctx, "parent_uuid", subvol->parent_uuid);
	fmt_print(fctx, "received_uuid", subvol->received_uuid);
	fmt_print(fctx, "otime", subvol->otime);
	fmt_print(fctx, "ID", subvol->id);
	fmt_print(fctx, "gen", subvol->generation);
	fmt_print(fctx, "cgen", subvol->otransid);
	fmt_print(fctx, "parent", subvol->parent_id);
	fmt_print(fctx, "top level", subvol->parent_id);

	fmt_print_start_group(fctx, "flags", JSON_TYPE_ARRAY);
	if (subvol->flags & BTRFS_ROOT_SUBVOL_RDONLY)
		fmt_print(fctx, "flag-list-item", "readonly");
	fmt_print_end_group(fctx, "flags");

	fmt_print(fctx, "stransid", subvol->stransid);
	fmt_print(fctx, "stime", subvol->stime);
	fmt_print(fctx, "rtransid", subvol->rtransid);
	fmt_print(fctx, "rtime", subvol->rtime);
}

static void print_subvolume_show_quota_json(struct format_ctx *fctx,
					     const struct btrfs_util_subvolume_info *subvol,
					     const struct btrfs_qgroup_stats *stats)
{
	fmt_print_start_group(fctx, "qgroup", JSON_TYPE_MAP);
	fmt_print(fctx, "quota-qgroup", 0, subvol->id);

	fmt_print_start_group(fctx, "limit", JSON_TYPE_MAP);
	fmt_print(fctx, "quota-ref", stats->limit.max_referenced);
	fmt_print(fctx, "quota-excl", stats->limit.max_exclusive);
	fmt_print_end_group(fctx, "limit");

	fmt_print_start_group(fctx, "usage", JSON_TYPE_MAP);
	fmt_print(fctx, "quota-ref", stats->info.referenced);
	fmt_print(fctx, "quota-excl", stats->info.exclusive);
	fmt_print_end_group(fctx, "usage");

	fmt_print_end_group(fctx, "qgroup");
}

static const char * const cmd_subvolume_show_usage[] = {
	"btrfs subvolume show [options] <path>",
	"Show more information about the subvolume (UUIDs, generations, times, snapshots)",
	"Show more information about the subvolume (UUIDs, generations, times, snapshots).",
	"The subvolume can be specified by path, or by root id or UUID that are",
	"looked up relative to the given path",
	"",
	OPTLINE("-r|--rootid ID", "root id of the subvolume"),
	OPTLINE("-u|--uuid UUID", "UUID of the subvolum"),
	HELPINFO_UNITS_SHORT_LONG,
#if EXPERIMENTAL
	HELPINFO_INSERT_GLOBALS,
	HELPINFO_INSERT_FORMAT,
#endif
	NULL
};

static int cmd_subvolume_show(const struct cmd_struct *cmd, int argc, char **argv)
{
	char uuidparse[BTRFS_UUID_UNPARSED_SIZE];
	char *fullpath = NULL;
	int fd = -1;
	int ret = 1;
	int by_rootid = 0;
	int by_uuid = 0;
	u64 rootid_arg = 0;
	u8 uuid_arg[BTRFS_UUID_SIZE];
	struct btrfs_util_subvolume_iterator *iter;
	struct btrfs_util_subvolume_info subvol;
	char *subvol_path = NULL;
	const char *subvol_name = NULL;
	enum btrfs_util_error err;
	struct btrfs_qgroup_stats stats;
	unsigned int unit_mode;
	struct format_ctx fctx;

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
		usage(cmd, 1);
	}

	fullpath = realpath(argv[optind], NULL);
	if (!fullpath) {
		error("cannot find real path for '%s': %m", argv[optind]);
		goto out;
	}

	fd = btrfs_open_file_or_dir(fullpath);
	if (fd < 0) {
		ret = fd;
		goto out;
	}

	if (by_uuid) {
		err = btrfs_util_subvolume_iter_create_fd(fd, BTRFS_FS_TREE_OBJECTID,
							  0, &iter);
		if (err) {
			error_btrfs_util(err);
			goto out;
		}

		for (;;) {
			err = btrfs_util_subvolume_iter_next_info(iter, &subvol_path, &subvol);
			if (err == BTRFS_UTIL_ERROR_STOP_ITERATION) {
				uuid_unparse(uuid_arg, uuidparse);
				error("can't find uuid '%s' on '%s'", uuidparse,
				      fullpath);
				btrfs_util_subvolume_iter_destroy(iter);
				goto out;
			} else if (err) {
				error_btrfs_util(err);
				btrfs_util_subvolume_iter_destroy(iter);
				goto out;
			}

			if (uuid_compare(subvol.uuid, uuid_arg) == 0)
				break;

			free(subvol_path);
			subvol_path = NULL;
		}
		btrfs_util_subvolume_iter_destroy(iter);
	} else {
		/*
		 * If !by_rootid, rootid_arg = 0, which means find the
		 * subvolume ID of the fd and use that.
		 */
		err = btrfs_util_subvolume_get_info_fd(fd, rootid_arg, &subvol);
		if (err) {
			error_btrfs_util(err);
			goto out;
		}

		err = btrfs_util_subvolume_get_path_fd(fd, subvol.id, &subvol_path);
		if (err) {
			error_btrfs_util(err);
			goto out;
		}

	}

	if (subvol.id == BTRFS_FS_TREE_OBJECTID) {
		free(subvol_path);
		subvol_path = strdup("/");
		subvol_name = "<FS_TREE>";
	} else {
		subvol_name = path_basename(subvol_path);
	}

	if (bconf.output_format == CMD_FORMAT_JSON) {
		fmt_start(&fctx, btrfs_subvolume_rowspec, 1, 0);
		fmt_print_start_group(&fctx, subvol_path, JSON_TYPE_MAP);
		print_subvolume_show_json(&fctx, &subvol, subvol_path, subvol_name);
	} else {
		print_subvolume_show_text(&subvol, subvol_path, subvol_name);
	}

	/* print the snapshots of the given subvol if any*/
	if (bconf.output_format == CMD_FORMAT_JSON)
		fmt_print_start_group(&fctx, "snapshots", JSON_TYPE_ARRAY);
	else
		pr_verbose(LOG_DEFAULT, "\tSnapshot(s):\n");

	err = btrfs_util_subvolume_iter_create_fd(fd, BTRFS_FS_TREE_OBJECTID, 0, &iter);

	for (;;) {
		struct btrfs_util_subvolume_info subvol2;
		char *path;

		err = btrfs_util_subvolume_iter_next_info(iter, &path, &subvol2);
		if (err == BTRFS_UTIL_ERROR_STOP_ITERATION) {
			break;
		} else if (err) {
			error_btrfs_util(err);
			btrfs_util_subvolume_iter_destroy(iter);
			goto out2;
		}

		if (uuid_compare(subvol2.parent_uuid, subvol.uuid) == 0) {
			if (bconf.output_format == CMD_FORMAT_JSON)
				fmt_print(&fctx, "snapshot-list-item", path);
			else
				pr_verbose(LOG_DEFAULT, "\t\t\t\t%s\n", path);
		}

		free(path);
	}

	if (bconf.output_format == CMD_FORMAT_JSON)
		fmt_print_end_group(&fctx, "snapshots");

	btrfs_util_subvolume_iter_destroy(iter);

	ret = btrfs_qgroup_query(fd, subvol.id, &stats);
	if (ret == -ENOTTY) {
		/* Quota information not available, not fatal */
		if (bconf.output_format == CMD_FORMAT_TEXT)
			pr_verbose(LOG_DEFAULT, "\tQuota group:\t\tn/a\n");
		ret = 0;
		goto out2;
	}

	if (ret) {
		error("quota query failed: %m");
		goto out2;
	}

	if (bconf.output_format == CMD_FORMAT_JSON)
		print_subvolume_show_quota_json(&fctx, &subvol, &stats);
	else
		print_subvolume_show_quota_text(&subvol, &stats, unit_mode);

out2:
	if (bconf.output_format == CMD_FORMAT_JSON) {
		fmt_print_end_group(&fctx, subvol_path);
		fmt_end(&fctx);
	}

out:
	free(subvol_path);
	close(fd);
	free(fullpath);
	return !!ret;
}
#if EXPERIMENTAL
static DEFINE_COMMAND_WITH_FLAGS(subvolume_show, "show", CMD_FORMAT_JSON);
#else
DEFINE_SIMPLE_COMMAND(subvolume_show, "show");
#endif

static const char * const cmd_subvolume_sync_usage[] = {
	"btrfs subvolume sync <path> [<subvolid>...]",
	"Wait until given subvolume(s) are completely removed from the filesystem.",
	"Wait until given subvolume(s) are completely removed from the filesystem",
	"after deletion.",
	"If no subvolume id is given, wait until all current deletion requests",
	"are completed, but do not wait for subvolumes deleted meanwhile.",
	"The status of subvolume ids is checked periodically.",
	"",
	OPTLINE("-s <N>", "sleep N seconds between checks (default: 1)"),
	NULL
};

static int cmd_subvolume_sync(const struct cmd_struct *cmd, int argc, char **argv)
{
	int fd = -1;
	int ret = 1;
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

	fd = btrfs_open_dir(argv[optind]);
	if (fd < 0) {
		ret = 1;
		goto out;
	}
	optind++;

	id_count = argc - optind;
	if (!id_count) {
		err = btrfs_util_subvolume_list_deleted_fd(fd, &ids, &id_count);
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
			error_mem(NULL);
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
	close(fd);

	return !!ret;
}
static DEFINE_SIMPLE_COMMAND(subvolume_sync, "sync");

static const char subvolume_cmd_group_info[] =
"manage subvolumes: create, delete, list, etc";

static const struct cmd_group subvolume_cmd_group = {
	subvolume_cmd_group_usage, subvolume_cmd_group_info, {
		&cmd_struct_subvolume_create,
		&cmd_struct_subvolume_delete,
		&cmd_struct_subvolume_list,
		&cmd_struct_subvolume_snapshot,
		&cmd_struct_subvolume_get_default,
		&cmd_struct_subvolume_set_default,
		&cmd_struct_subvolume_find_new,
		&cmd_struct_subvolume_show,
		&cmd_struct_subvolume_sync,
		NULL
	}
};

DEFINE_GROUP_COMMAND_TOKEN(subvolume);
