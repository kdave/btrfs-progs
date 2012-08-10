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

#define _GNU_SOURCE

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

#include <uuid/uuid.h>

#include "ctree.h"
#include "ioctl.h"
#include "commands.h"
#include "list.h"

#include "send.h"
#include "send-utils.h"

static int g_verbose = 0;

struct btrfs_send {
	int send_fd;
	int dump_fd;
	int mnt_fd;

	u64 *clone_sources;
	u64 clone_sources_count;

	char *root_path;
	struct subvol_uuid_search sus;
};

int find_mount_root(const char *path, char **mount_root)
{
	FILE *mnttab;
	int fd;
	struct mntent *ent;
	int len;
	int longest_matchlen = 0;
	char *longest_match = NULL;

	fd = open(path, O_RDONLY | O_NOATIME);
	if (fd < 0)
		return -errno;
	close(fd);

	mnttab = fopen("/etc/mtab", "r");
	while ((ent = getmntent(mnttab))) {
		len = strlen(ent->mnt_dir);
		if (strncmp(ent->mnt_dir, path, len) == 0) {
			/* match found */
			if (longest_matchlen < len) {
				free(longest_match);
				longest_matchlen = len;
				longest_match = strdup(ent->mnt_dir);
			}
		}
	}

	*mount_root = realpath(longest_match, NULL);
	free(longest_match);

	return 0;
}

static int get_root_id(struct btrfs_send *s, const char *path, u64 *root_id)
{
	struct subvol_info *si;

	si = subvol_uuid_search(&s->sus, 0, NULL, 0, path,
			subvol_search_by_path);
	if (!si)
		return -ENOENT;
	*root_id = si->root_id;
	return 0;
}

static struct subvol_info *get_parent(struct btrfs_send *s, u64 root_id)
{
	struct subvol_info *si;

	si = subvol_uuid_search(&s->sus, root_id, NULL, 0, NULL,
			subvol_search_by_root_id);
	if (!si)
		return NULL;

	si = subvol_uuid_search(&s->sus, 0, si->parent_uuid, 0, NULL,
			subvol_search_by_uuid);
	if (!si)
		return NULL;
	return si;
}

static int find_good_parent(struct btrfs_send *s, u64 root_id, u64 *found)
{
	int ret;
	struct subvol_info *parent;
	struct subvol_info *parent2;
	struct subvol_info *best_parent = NULL;
	__s64 tmp;
	u64 best_diff = (u64)-1;
	int i;

	parent = get_parent(s, root_id);
	if (!parent) {
		ret = -ENOENT;
		goto out;
	}

	for (i = 0; i < s->clone_sources_count; i++) {
		if (s->clone_sources[i] == parent->root_id) {
			best_parent = parent;
			goto out_found;
		}
	}

	for (i = 0; i < s->clone_sources_count; i++) {
		parent2 = get_parent(s, s->clone_sources[i]);
		if (parent2 != parent)
			continue;

		parent2 = subvol_uuid_search(&s->sus, s->clone_sources[i], NULL,
				0, NULL, subvol_search_by_root_id);

		tmp = parent2->ctransid - parent->ctransid;
		if (tmp < 0)
			tmp *= -1;
		if (tmp < best_diff) {
			best_parent = parent;
			best_diff = tmp;
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
	return ret;
}

static void add_clone_source(struct btrfs_send *s, u64 root_id)
{
	s->clone_sources = realloc(s->clone_sources,
		sizeof(*s->clone_sources) * (s->clone_sources_count + 1));
	s->clone_sources[s->clone_sources_count++] = root_id;
}

static int write_buf(int fd, const void *buf, int size)
{
	int ret;
	int pos = 0;

	while (pos < size) {
		ret = write(fd, (char*)buf + pos, size - pos);
		if (ret < 0) {
			ret = -errno;
			fprintf(stderr, "ERROR: failed to dump stream. %s",
					strerror(-ret));
			goto out;
		}
		if (!ret) {
			ret = -EIO;
			fprintf(stderr, "ERROR: failed to dump stream. %s",
					strerror(-ret));
			goto out;
		}
		pos += ret;
	}
	ret = 0;

out:
	return ret;
}

static void *dump_thread(void *arg_)
{
	int ret;
	struct btrfs_send *s = (struct btrfs_send*)arg_;
	char buf[4096];
	int readed;

	while (1) {
		readed = read(s->send_fd, buf, sizeof(buf));
		if (readed < 0) {
			ret = -errno;
			fprintf(stderr, "ERROR: failed to read stream from "
					"kernel. %s\n", strerror(-ret));
			goto out;
		}
		if (!readed) {
			ret = 0;
			goto out;
		}
		ret = write_buf(s->dump_fd, buf, readed);
		if (ret < 0)
			goto out;
	}

out:
	if (ret < 0) {
		exit(-ret);
	}

	return ERR_PTR(ret);
}

static int do_send(struct btrfs_send *send, u64 root_id, u64 parent_root)
{
	int ret;
	pthread_t t_read;
	pthread_attr_t t_attr;
	struct btrfs_ioctl_send_args io_send;
	struct subvol_info *si;
	void *t_err = NULL;
	int subvol_fd = -1;
	int pipefd[2];

	si = subvol_uuid_search(&send->sus, root_id, NULL, 0, NULL,
			subvol_search_by_root_id);
	if (!si) {
		ret = -ENOENT;
		fprintf(stderr, "ERROR: could not find subvol info for %llu",
				root_id);
		goto out;
	}

	subvol_fd = openat(send->mnt_fd, si->path, O_RDONLY | O_NOATIME);
	if (subvol_fd < 0) {
		ret = -errno;
		fprintf(stderr, "ERROR: open %s failed. %s\n", si->path,
				strerror(-ret));
		goto out;
	}

	ret = pthread_attr_init(&t_attr);

	ret = pipe(pipefd);
	if (ret < 0) {
		ret = -errno;
		fprintf(stderr, "ERROR: pipe failed. %s\n", strerror(-ret));
		goto out;
	}

	io_send.send_fd = pipefd[1];
	send->send_fd = pipefd[0];

	if (!ret)
		ret = pthread_create(&t_read, &t_attr, dump_thread,
					send);
	if (ret) {
		ret = -ret;
		fprintf(stderr, "ERROR: thread setup failed: %s\n",
			strerror(-ret));
		goto out;
	}

	io_send.clone_sources = (__u64*)send->clone_sources;
	io_send.clone_sources_count = send->clone_sources_count;
	io_send.parent_root = parent_root;
	ret = ioctl(subvol_fd, BTRFS_IOC_SEND, &io_send);
	if (ret) {
		ret = -errno;
		fprintf(stderr, "ERROR: send ioctl failed with %d: %s\n", ret,
			strerror(-ret));
		goto out;
	}
	if (g_verbose > 0)
		fprintf(stderr, "BTRFS_IOC_SEND returned %d\n", ret);

	if (g_verbose > 0)
		fprintf(stderr, "joining genl thread\n");

	close(pipefd[1]);
	pipefd[1] = 0;

	ret = pthread_join(t_read, &t_err);
	if (ret) {
		ret = -ret;
		fprintf(stderr, "ERROR: pthread_join failed: %s\n",
			strerror(-ret));
		goto out;
	}
	if (t_err) {
		ret = (long int)t_err;
		fprintf(stderr, "ERROR: failed to process send stream, ret=%ld "
			"(%s)\n", (long int)t_err, strerror(-ret));
		goto out;
	}

	pthread_attr_destroy(&t_attr);

	ret = 0;

out:
	if (subvol_fd != -1)
		close(subvol_fd);
	if (pipefd[0])
		close(pipefd[0]);
	if (pipefd[1])
		close(pipefd[1]);
	return ret;
}

static const char *get_subvol_name(struct btrfs_send *s, const char *full_path)
{
	int len = strlen(s->root_path);
	if (!len)
		return full_path;
	if (s->root_path[len - 1] != '/')
		len += 1;

	return full_path + len;
}

static int init_root_path(struct btrfs_send *s, const char *subvol)
{
	int ret = 0;

	if (s->root_path)
		goto out;

	ret = find_mount_root(subvol, &s->root_path);
	if (ret < 0) {
		ret = -EINVAL;
		fprintf(stderr, "ERROR: failed to determine mount point "
				"for %s\n", subvol);
		goto out;
	}

	s->mnt_fd = open(s->root_path, O_RDONLY | O_NOATIME);
	if (s->mnt_fd < 0) {
		ret = -errno;
		fprintf(stderr, "ERROR: can't open '%s': %s\n", s->root_path,
			strerror(-ret));
		goto out;
	}

	ret = subvol_uuid_search_init(s->mnt_fd, &s->sus);
	if (ret < 0) {
		fprintf(stderr, "ERROR: failed to initialize subvol search. "
				"%s\n", strerror(-ret));
		goto out;
	}

out:
	return ret;

}

static int is_subvol_ro(struct btrfs_send *s, char *subvol)
{
	int ret;
	u64 flags;
	int fd = -1;

	fd = openat(s->mnt_fd, subvol, O_RDONLY | O_NOATIME);
	if (fd < 0) {
		ret = -errno;
		fprintf(stderr, "ERROR: failed to open %s. %s\n",
				subvol, strerror(-ret));
		goto out;
	}

	ret = ioctl(fd, BTRFS_IOC_SUBVOL_GETFLAGS, &flags);
	if (ret < 0) {
		ret = -errno;
		fprintf(stderr, "ERROR: failed to get flags for subvolume. "
				"%s\n", strerror(-ret));
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

int cmd_send_start(int argc, char **argv)
{
	char *subvol = NULL;
	char c;
	int ret;
	char *outname = NULL;
	struct btrfs_send send;
	u32 i;
	char *mount_root = NULL;
	char *snapshot_parent = NULL;
	u64 root_id;
	u64 parent_root_id = 0;

	memset(&send, 0, sizeof(send));
	send.dump_fd = fileno(stdout);

	while ((c = getopt(argc, argv, "vf:i:p:")) != -1) {
		switch (c) {
		case 'v':
			g_verbose++;
			break;
		case 'i': {
			subvol = realpath(optarg, NULL);
			if (!subvol) {
				ret = -errno;
				fprintf(stderr, "ERROR: realpath %s failed. "
						"%s\n", optarg, strerror(-ret));
				goto out;
			}

			ret = init_root_path(&send, subvol);
			if (ret < 0)
				goto out;

			ret = get_root_id(&send, get_subvol_name(&send, subvol),
					&root_id);
			if (ret < 0) {
				fprintf(stderr, "ERROR: could not resolve "
						"root_id for %s\n", subvol);
				goto out;
			}
			add_clone_source(&send, root_id);
			free(subvol);
			break;
		}
		case 'f':
			outname = optarg;
			break;
		case 'p':
			snapshot_parent = realpath(optarg, NULL);
			if (!snapshot_parent) {
				ret = -errno;
				fprintf(stderr, "ERROR: realpath %s failed. "
						"%s\n", optarg, strerror(-ret));
				goto out;
			}
			break;
		case '?':
		default:
			fprintf(stderr, "ERROR: send args invalid.\n");
			return 1;
		}
	}

	if (optind == argc) {
		fprintf(stderr, "ERROR: send needs path to snapshot\n");
		return 1;
	}

	if (outname != NULL) {
		send.dump_fd = creat(outname, 0600);
		if (send.dump_fd == -1) {
			ret = -errno;
			fprintf(stderr, "ERROR: can't create '%s': %s\n",
					outname, strerror(-ret));
			goto out;
		}
	}

	/* use first send subvol to determine mount_root */
	subvol = argv[optind];

	subvol = realpath(argv[optind], NULL);
	if (!subvol) {
		ret = -errno;
		fprintf(stderr, "ERROR: unable to resolve %s\n", argv[optind]);
		goto out;
	}

	ret = init_root_path(&send, subvol);
	if (ret < 0)
		goto out;

	if (snapshot_parent != NULL) {
		ret = get_root_id(&send,
				get_subvol_name(&send, snapshot_parent),
				&parent_root_id);
		if (ret < 0) {
			fprintf(stderr, "ERROR: could not resolve root_id "
					"for %s\n", snapshot_parent);
			goto out;
		}

		add_clone_source(&send, parent_root_id);
	}

	for (i = optind; i < argc; i++) {
		subvol = realpath(argv[i], NULL);
		if (!subvol) {
			ret = -errno;
			fprintf(stderr, "ERROR: unable to resolve %s\n", argv[i]);
			goto out;
		}

		ret = find_mount_root(subvol, &mount_root);
		if (ret < 0) {
			fprintf(stderr, "ERROR: find_mount_root failed on %s: "
					"%s\n", subvol,
				strerror(-ret));
			goto out;
		}
		if (strcmp(send.root_path, mount_root) != 0) {
			ret = -EINVAL;
			fprintf(stderr, "ERROR: all subvols must be from the "
					"same fs.\n");
			goto out;
		}
		free(mount_root);

		ret = is_subvol_ro(&send, subvol);
		if (ret < 0)
			goto out;
		if (!ret) {
			ret = -EINVAL;
			fprintf(stderr, "ERROR: %s is not read-only.\n",
					subvol);
			goto out;
		}
		free(subvol);
	}

	for (i = optind; i < argc; i++) {
		subvol = argv[i];

		fprintf(stderr, "At subvol %s\n", subvol);

		subvol = realpath(subvol, NULL);
		if (!subvol) {
			ret = -errno;
			fprintf(stderr, "ERROR: realpath %s failed. "
					"%s\n", argv[i], strerror(-ret));
			goto out;
		}

		ret = get_root_id(&send, get_subvol_name(&send, subvol),
				&root_id);
		if (ret < 0) {
			fprintf(stderr, "ERROR: could not resolve root_id "
					"for %s\n", subvol);
			goto out;
		}

		if (!parent_root_id) {
			ret = find_good_parent(&send, root_id, &parent_root_id);
			if (ret < 0)
				parent_root_id = 0;
		}

		ret = is_subvol_ro(&send, subvol);
		if (ret < 0)
			goto out;
		if (!ret) {
			ret = -EINVAL;
			fprintf(stderr, "ERROR: %s is not read-only.\n",
					subvol);
			goto out;
		}

		ret = do_send(&send, root_id, parent_root_id);
		if (ret < 0)
			goto out;

		/* done with this subvol, so add it to the clone sources */
		add_clone_source(&send, root_id);

		parent_root_id = 0;
		free(subvol);
	}

	ret = 0;

out:
	if (send.mnt_fd >= 0)
		close(send.mnt_fd);
	return ret;
}

static const char * const send_cmd_group_usage[] = {
	"btrfs send <command> <args>",
	NULL
};

static const char * const cmd_send_usage[] = {
	"btrfs send [-v] [-i <subvol>] [-p <parent>] <subvol>",
	"Send the subvolume to stdout.",
	"Sends the subvolume specified by <subvol> to stdout.",
	"By default, this will send the whole subvolume. To do",
	"an incremental send, one or multiple '-i <clone_source>'",
	"arguments have to be specified. A 'clone source' is",
	"a subvolume that is known to exist on the receiving",
	"side in exactly the same state as on the sending side.\n",
	"Normally, a good snapshot parent is searched automatically",
	"in the list of 'clone sources'. To override this, use",
	"'-p <parent>' to manually specify a snapshot parent.",
	"A manually specified snapshot parent is also regarded",
	"as 'clone source'.\n",
	"-v               Enable verbose debug output. Each",
	"                 occurrency of this option increases the",
	"                 verbose level more.",
	"-i <subvol>      Informs btrfs send that this subvolume,",
	"                 can be taken as 'clone source'. This can",
	"                 be used for incremental sends.",
	"-p <subvol>      Disable automatic snaphot parent",
	"                 determination and use <subvol> as parent.",
	"                 This subvolume is also added to the list",
	"                 of 'clone sources' (see -i).",
	"-f <outfile>     Output is normally written to stdout.",
	"                 To write to a file, use this option.",
	"                 An alternative would be to use pipes.",
	NULL
};

const struct cmd_group send_cmd_group = {
	send_cmd_group_usage, NULL, {
		{ "send", cmd_send_start, cmd_send_usage, NULL, 0 },
		{ 0, 0, 0, 0, 0 },
        },
};

int cmd_send(int argc, char **argv)
{
	return cmd_send_start(argc, argv);
}
