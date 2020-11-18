/*
 * Copyright (C) 2012 Alexander Block.  All rights reserved.
 *
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
#include <stdint.h>
#include <dirent.h>
#include <fcntl.h>
#include <pthread.h>
#include <math.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/ioctl.h>
#include <libgen.h>
#include <mntent.h>
#include <assert.h>
#include <getopt.h>
#include <uuid/uuid.h>
#include <limits.h>

#include "kernel-shared/ctree.h"
#include "ioctl.h"
#include "cmds/commands.h"
#include "kernel-lib/list.h"
#include "common/utils.h"

#include "send.h"
#include "common/send-utils.h"
#include "common/help.h"
#include "common/path-utils.h"

#define SEND_BUFFER_SIZE	SZ_64K


struct btrfs_send {
	int send_fd;
	int dump_fd;
	int mnt_fd;

	u64 *clone_sources;
	u64 clone_sources_count;

	char *root_path;
	struct subvol_uuid_search sus;
};

static int get_root_id(struct btrfs_send *sctx, const char *path, u64 *root_id)
{
	struct subvol_info *si;

	si = subvol_uuid_search(&sctx->sus, 0, NULL, 0, path,
			subvol_search_by_path);
	if (IS_ERR_OR_NULL(si)) {
		if (!si)
			return -ENOENT;
		else
			return PTR_ERR(si);
	}
	*root_id = si->root_id;
	free(si->path);
	free(si);
	return 0;
}

static struct subvol_info *get_parent(struct btrfs_send *sctx, u64 root_id)
{
	struct subvol_info *si_tmp;
	struct subvol_info *si;

	si_tmp = subvol_uuid_search(&sctx->sus, root_id, NULL, 0, NULL,
			subvol_search_by_root_id);
	if (IS_ERR_OR_NULL(si_tmp))
		return si_tmp;

	si = subvol_uuid_search(&sctx->sus, 0, si_tmp->parent_uuid, 0, NULL,
			subvol_search_by_uuid);
	free(si_tmp->path);
	free(si_tmp);
	return si;
}

static int find_good_parent(struct btrfs_send *sctx, u64 root_id, u64 *found)
{
	int ret;
	struct subvol_info *parent = NULL;
	struct subvol_info *parent2 = NULL;
	struct subvol_info *best_parent = NULL;
	u64 best_diff = (u64)-1;
	int i;

	parent = get_parent(sctx, root_id);
	if (IS_ERR_OR_NULL(parent)) {
		if (!parent)
			ret = -ENOENT;
		else
			ret = PTR_ERR(parent);
		goto out;
	}

	for (i = 0; i < sctx->clone_sources_count; i++) {
		if (sctx->clone_sources[i] == parent->root_id) {
			best_parent = parent;
			parent = NULL;
			goto out_found;
		}
	}

	for (i = 0; i < sctx->clone_sources_count; i++) {
		s64 tmp;

		parent2 = get_parent(sctx, sctx->clone_sources[i]);
		if (IS_ERR_OR_NULL(parent2))
			continue;
		if (parent2->root_id != parent->root_id) {
			free(parent2->path);
			free(parent2);
			parent2 = NULL;
			continue;
		}

		free(parent2->path);
		free(parent2);
		parent2 = subvol_uuid_search(&sctx->sus,
				sctx->clone_sources[i], NULL, 0, NULL,
				subvol_search_by_root_id);
		if (IS_ERR_OR_NULL(parent2)) {
			if (!parent2)
				ret = -ENOENT;
			else
				ret = PTR_ERR(parent2);
			goto out;
		}
		tmp = parent2->ctransid - parent->ctransid;
		if (tmp < 0)
			tmp = -tmp;
		if (tmp < best_diff) {
			if (best_parent) {
				free(best_parent->path);
				free(best_parent);
			}
			best_parent = parent2;
			parent2 = NULL;
			best_diff = tmp;
		} else {
			free(parent2->path);
			free(parent2);
			parent2 = NULL;
		}
	}

	if (!best_parent) {
		ret = -ENOENT;
		goto out;
	}

out_found:
	*found = best_parent->root_id;
	ret = 0;

out:
	if (parent) {
		free(parent->path);
		free(parent);
	}
	if (best_parent) {
		free(best_parent->path);
		free(best_parent);
	}
	return ret;
}

static int add_clone_source(struct btrfs_send *sctx, u64 root_id)
{
	void *tmp;

	tmp = sctx->clone_sources;
	sctx->clone_sources = realloc(sctx->clone_sources,
		sizeof(*sctx->clone_sources) * (sctx->clone_sources_count + 1));

	if (!sctx->clone_sources) {
		free(tmp);
		return -ENOMEM;
	}
	sctx->clone_sources[sctx->clone_sources_count++] = root_id;

	return 0;
}

static void *read_sent_data(void *arg)
{
	int ret;
	struct btrfs_send *sctx = (struct btrfs_send*)arg;

	while (1) {
		ssize_t sbytes;

		/* Source is a pipe, output is either file or stdout */
		sbytes = splice(sctx->send_fd, NULL, sctx->dump_fd,
				NULL, SEND_BUFFER_SIZE, SPLICE_F_MORE);
		if (sbytes < 0) {
			ret = -errno;
			error("failed to read stream from kernel: %m");
			goto out;
		}
		if (!sbytes) {
			ret = 0;
			goto out;
		}
	}

out:
	if (ret < 0)
		exit(-ret);

	return ERR_PTR(ret);
}

static int do_send(struct btrfs_send *send, u64 parent_root_id,
		   int is_first_subvol, int is_last_subvol, const char *subvol,
		   u64 flags)
{
	int ret;
	pthread_t t_read;
	struct btrfs_ioctl_send_args io_send;
	void *t_err = NULL;
	int subvol_fd = -1;
	int pipefd[2] = {-1, -1};

	subvol_fd = openat(send->mnt_fd, subvol, O_RDONLY | O_NOATIME);
	if (subvol_fd < 0) {
		ret = -errno;
		error("cannot open %s: %m", subvol);
		goto out;
	}

	ret = pipe(pipefd);
	if (ret < 0) {
		ret = -errno;
		error("pipe failed: %m");
		goto out;
	}

	memset(&io_send, 0, sizeof(io_send));
	io_send.send_fd = pipefd[1];
	send->send_fd = pipefd[0];

	if (!ret)
		ret = pthread_create(&t_read, NULL, read_sent_data, send);
	if (ret) {
		ret = -ret;
		errno = -ret;
		error("thread setup failed: %m");
		goto out;
	}

	io_send.flags = flags;
	io_send.clone_sources = (__u64*)send->clone_sources;
	io_send.clone_sources_count = send->clone_sources_count;
	io_send.parent_root = parent_root_id;
	if (!is_first_subvol)
		io_send.flags |= BTRFS_SEND_FLAG_OMIT_STREAM_HEADER;
	if (!is_last_subvol)
		io_send.flags |= BTRFS_SEND_FLAG_OMIT_END_CMD;
	ret = ioctl(subvol_fd, BTRFS_IOC_SEND, &io_send);
	if (ret < 0) {
		ret = -errno;
		error("send ioctl failed with %d: %m", ret);
		if (ret == -EINVAL && (!is_first_subvol || !is_last_subvol))
			fprintf(stderr,
				"Try upgrading your kernel or don't use -e.\n");
		goto out;
	}
	if (bconf.verbose > 1)
		fprintf(stderr, "BTRFS_IOC_SEND returned %d\n", ret);

	if (bconf.verbose > 1)
		fprintf(stderr, "joining genl thread\n");

	close(pipefd[1]);
	pipefd[1] = -1;

	ret = pthread_join(t_read, &t_err);
	if (ret) {
		ret = -ret;
		errno = -ret;
		error("pthread_join failed: %m");
		goto out;
	}
	if (t_err) {
		ret = (long int)t_err;
		error("failed to process send stream, ret=%ld (%s)",
				(long int)t_err, strerror(-ret));
		goto out;
	}

	ret = 0;

out:
	if (subvol_fd != -1)
		close(subvol_fd);
	if (pipefd[0] != -1)
		close(pipefd[0]);
	if (pipefd[1] != -1)
		close(pipefd[1]);
	return ret;
}

static int init_root_path(struct btrfs_send *sctx, const char *subvol)
{
	int ret = 0;

	if (sctx->root_path)
		goto out;

	ret = find_mount_root(subvol, &sctx->root_path);
	if (ret < 0) {
		errno = -ret;
		error("failed to determine mount point for %s: %m", subvol);
		ret = -EINVAL;
		goto out;
	}
	if (ret > 0) {
		error("%s doesn't belong to btrfs mount point", subvol);
		ret = -EINVAL;
		goto out;
	}

	sctx->mnt_fd = open(sctx->root_path, O_RDONLY | O_NOATIME);
	if (sctx->mnt_fd < 0) {
		ret = -errno;
		error("cannot open '%s': %m", sctx->root_path);
		goto out;
	}

	ret = subvol_uuid_search_init(sctx->mnt_fd, &sctx->sus);
	if (ret < 0) {
		errno = -ret;
		error("failed to initialize subvol search: %m");
		goto out;
	}

out:
	return ret;

}

static int is_subvol_ro(struct btrfs_send *sctx, const char *subvol)
{
	int ret;
	u64 flags;
	int fd = -1;

	fd = openat(sctx->mnt_fd, subvol, O_RDONLY | O_NOATIME);
	if (fd < 0) {
		ret = -errno;
		error("cannot open %s: %m", subvol);
		goto out;
	}

	ret = ioctl(fd, BTRFS_IOC_SUBVOL_GETFLAGS, &flags);
	if (ret < 0) {
		ret = -errno;
		error("failed to get flags for subvolume %s: %m", subvol);
		goto out;
	}

	if (flags & BTRFS_SUBVOL_RDONLY)
		ret = 1;
	else
		ret = 0;

out:
	if (fd != -1)
		close(fd);

	return ret;
}

static int set_root_info(struct btrfs_send *sctx, const char *subvol,
		u64 *root_id)
{
	int ret;

	ret = init_root_path(sctx, subvol);
	if (ret < 0)
		goto out;

	ret = get_root_id(sctx, subvol_strip_mountpoint(sctx->root_path, subvol),
		root_id);
	if (ret < 0) {
		error("cannot resolve rootid for %s", subvol);
		goto out;
	}

out:
	return ret;
}

static void free_send_info(struct btrfs_send *sctx)
{
	if (sctx->mnt_fd >= 0) {
		close(sctx->mnt_fd);
		sctx->mnt_fd = -1;
	}
	free(sctx->root_path);
	sctx->root_path = NULL;
	subvol_uuid_search_finit(&sctx->sus);
}

static const char * const cmd_send_usage[] = {
	"btrfs send [-ve] [-p <parent>] [-c <clone-src>] [-f <outfile>] <subvol> [<subvol>...]",
	"Send the subvolume(s) to stdout.",
	"Sends the subvolume(s) specified by <subvol> to stdout.",
	"<subvol> should be read-only here.",
	"By default, this will send the whole subvolume. To do an incremental",
	"send, use '-p <parent>'. If you want to allow btrfs to clone from",
	"any additional local snapshots, use '-c <clone-src>' (multiple times",
	"where applicable). You must not specify clone sources unless you",
	"guarantee that these snapshots are exactly in the same state on both",
	"sides, the sender and the receiver. It is allowed to omit the",
	"'-p <parent>' option when '-c <clone-src>' options are given, in",
	"which case 'btrfs send' will determine a suitable parent among the",
	"clone sources itself.",
	"",
	"-e               If sending multiple subvols at once, use the new",
	"                 format and omit the end-cmd between the subvols.",
	"-p <parent>      Send an incremental stream from <parent> to",
	"                 <subvol>.",
	"-c <clone-src>   Use this snapshot as a clone source for an ",
	"                 incremental send (multiple allowed)",
	"-f <outfile>     Output is normally written to stdout. To write to",
	"                 a file, use this option. An alternative would be to",
	"                 use pipes.",
	"--no-data        send in NO_FILE_DATA mode, Note: the output stream",
	"                 does not contain any file data and thus cannot be used",
	"                 to transfer changes. This mode is faster and useful to",
	"                 show the differences in metadata.",
	"-v|--verbose     deprecated, alias for global -v option",
	"-q|--quiet       deprecated, alias for global -q option",
	HELPINFO_INSERT_GLOBALS,
	HELPINFO_INSERT_VERBOSE,
	HELPINFO_INSERT_QUIET,
	NULL
};

static int cmd_send(const struct cmd_struct *cmd, int argc, char **argv)
{
	char *subvol = NULL;
	int ret;
	char outname[PATH_MAX];
	struct btrfs_send send;
	u32 i;
	char *mount_root = NULL;
	char *snapshot_parent = NULL;
	u64 root_id = 0;
	u64 parent_root_id = 0;
	int full_send = 1;
	int new_end_cmd_semantic = 0;
	u64 send_flags = 0;

	memset(&send, 0, sizeof(send));
	send.dump_fd = fileno(stdout);
	outname[0] = 0;

	/*
	 * For send, verbose default is 1 (insteasd of 0) for historical reasons,
	 * changing may break scripts that expect the 'At subvol' message. But do
	 * it only when bconf.verbose is unset (-1) and also adjust the value,
	 * if global verbose is already set.
	 */
	if (bconf.verbose == BTRFS_BCONF_UNSET)
		bconf.verbose = 1;
	else if (bconf.verbose > BTRFS_BCONF_QUIET)
		bconf.verbose++;

	optind = 0;
	while (1) {
		enum { GETOPT_VAL_SEND_NO_DATA = 256 };
		static const struct option long_options[] = {
			{ "verbose", no_argument, NULL, 'v' },
			{ "quiet", no_argument, NULL, 'q' },
			{ "no-data", no_argument, NULL, GETOPT_VAL_SEND_NO_DATA },
			{ NULL, 0, NULL, 0 }
		};
		int c = getopt_long(argc, argv, "vqec:f:i:p:", long_options, NULL);

		if (c < 0)
			break;

		switch (c) {
		case 'v':
			bconf_be_verbose();
			break;
		case 'q':
			bconf_be_quiet();
			break;
		case 'e':
			new_end_cmd_semantic = 1;
			break;
		case 'c':
			subvol = realpath(optarg, NULL);
			if (!subvol) {
				ret = -errno;
				error("realpath %s failed: %m\n", optarg);
				goto out;
			}

			ret = set_root_info(&send, subvol, &root_id);
			if (ret < 0)
				goto out;

			ret = is_subvol_ro(&send, subvol);
			if (ret < 0)
				goto out;
			if (!ret) {
				ret = -EINVAL;
				error("cloned subvolume %s is not read-only", subvol);
				goto out;
			}

			ret = add_clone_source(&send, root_id);
			if (ret < 0) {
				errno = -ret;
				error("cannot add clone source: %m");
				goto out;
			}
			free(subvol);
			subvol = NULL;
			free_send_info(&send);
			full_send = 0;
			break;
		case 'f':
			if (arg_copy_path(outname, optarg, sizeof(outname))) {
				error("output file path too long (%zu)", strlen(optarg));
				ret = 1;
				goto out;
			}
			break;
		case 'p':
			if (snapshot_parent) {
				error("you cannot have more than one parent (-p)");
				ret = 1;
				goto out;
			}
			snapshot_parent = realpath(optarg, NULL);
			if (!snapshot_parent) {
				ret = -errno;
				error("realpath %s failed: %m", optarg);
				goto out;
			}

			ret = is_subvol_ro(&send, snapshot_parent);
			if (ret < 0)
				goto out;
			if (!ret) {
				ret = -EINVAL;
				error("parent subvolume %s is not read-only",
					snapshot_parent);
				goto out;
			}

			full_send = 0;
			break;
		case 'i':
			error("option -i was removed, use -c instead");
			ret = 1;
			goto out;
		case GETOPT_VAL_SEND_NO_DATA:
			send_flags |= BTRFS_SEND_FLAG_NO_FILE_DATA;
			break;
		default:
			usage_unknown_option(cmd, argv);
		}
	}

	if (check_argc_min(argc - optind, 1))
		return 1;

	if (outname[0]) {
		int tmpfd;

		/*
		 * Try to use an existing file first. Even if send runs as
		 * root, it might not have permissions to create file (eg. on a
		 * NFS) but it should still be able to use a pre-created file.
		 */
		tmpfd = open(outname, O_WRONLY | O_TRUNC);
		if (tmpfd < 0) {
			if (errno == ENOENT)
				tmpfd = open(outname,
					O_CREAT | O_WRONLY | O_TRUNC, 0600);
		}
		send.dump_fd = tmpfd;
		if (send.dump_fd == -1) {
			ret = -errno;
			error("cannot create '%s': %m", outname);
			goto out;
		}
	}

	if (isatty(send.dump_fd)) {
		error(
	    "not dumping send stream into a terminal, redirect it into a file");
		ret = 1;
		goto out;
	}

	/* use first send subvol to determine mount_root */
	subvol = realpath(argv[optind], NULL);
	if (!subvol) {
		ret = -errno;
		error("unable to resolve %s", argv[optind]);
		goto out;
	}

	ret = init_root_path(&send, subvol);
	if (ret < 0)
		goto out;

	if (snapshot_parent != NULL) {
		ret = get_root_id(&send,
			subvol_strip_mountpoint(send.root_path, snapshot_parent),
			&parent_root_id);
		if (ret < 0) {
			error("could not resolve rootid for %s", snapshot_parent);
			goto out;
		}

		ret = add_clone_source(&send, parent_root_id);
		if (ret < 0) {
			errno = -ret;
			error("cannot add clone source: %m");
			goto out;
		}
	}

	for (i = optind; i < argc; i++) {
		free(subvol);
		subvol = realpath(argv[i], NULL);
		if (!subvol) {
			ret = -errno;
			error("unable to resolve %s", argv[i]);
			goto out;
		}

		ret = find_mount_root(subvol, &mount_root);
		if (ret < 0) {
			errno = -ret;
			error("find_mount_root failed on %s: %m", subvol);
			goto out;
		}
		if (ret > 0) {
			error("%s does not belong to btrfs mount point",
				subvol);
			ret = -EINVAL;
			goto out;
		}
		if (strcmp(send.root_path, mount_root) != 0) {
			ret = -EINVAL;
			error("all subvolumes must be from the same filesystem");
			goto out;
		}
		free(mount_root);

		ret = is_subvol_ro(&send, subvol);
		if (ret < 0)
			goto out;
		if (!ret) {
			ret = -EINVAL;
			error("subvolume %s is not read-only", subvol);
			goto out;
		}
	}

	if ((send_flags & BTRFS_SEND_FLAG_NO_FILE_DATA) && bconf.verbose > 1)
		if (bconf.verbose > 1)
			fprintf(stderr, "Mode NO_FILE_DATA enabled\n");

	for (i = optind; i < argc; i++) {
		int is_first_subvol;
		int is_last_subvol;

		free(subvol);
		subvol = argv[i];

		if (bconf.verbose > BTRFS_BCONF_QUIET)
			fprintf(stderr, "At subvol %s\n", subvol);

		subvol = realpath(subvol, NULL);
		if (!subvol) {
			ret = -errno;
			error("realpath %s failed: %m", argv[i]);
			goto out;
		}

		if (!full_send && !snapshot_parent) {
			ret = set_root_info(&send, subvol, &root_id);
			if (ret < 0)
				goto out;

			ret = find_good_parent(&send, root_id, &parent_root_id);
			if (ret < 0) {
				error("parent determination failed for %lld",
					root_id);
				goto out;
			}
		}

		if (new_end_cmd_semantic) {
			/* require new kernel */
			is_first_subvol = (i == optind);
			is_last_subvol = (i == argc - 1);
		} else {
			/* be compatible to old and new kernel */
			is_first_subvol = 1;
			is_last_subvol = 1;
		}
		ret = do_send(&send, parent_root_id, is_first_subvol,
			      is_last_subvol, subvol, send_flags);
		if (ret < 0)
			goto out;

		if (!full_send && !snapshot_parent) {
			/* done with this subvol, so add it to the clone sources */
			ret = add_clone_source(&send, root_id);
			if (ret < 0) {
				errno = -ret;
				error("cannot add clone source: %m");
				goto out;
			}
			free_send_info(&send);
		}
	}

	ret = 0;

out:
	free(subvol);
	free(snapshot_parent);
	free(send.clone_sources);
	free_send_info(&send);
	return !!ret;
}
DEFINE_SIMPLE_COMMAND(send, "send");
