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

#include <sys/types.h>
#include <sys/stat.h>
#include <unistd.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdarg.h>
#include <getopt.h>
#include <fcntl.h>
#include <dirent.h>

#include <sys/ioctl.h>
#include <linux/fs.h>
#include <linux/fiemap.h>

#include "utils.h"
#include "commands.h"
#include "kerncompat.h"
#include "rbtree.h"

static int summarize = 0;
static unsigned unit_mode = UNITS_RAW;
static char path[PATH_MAX] = { 0, };
static char *pathp = path;
static char *path_max = &path[PATH_MAX - 1];

/* Track which inodes we've seen for the purposes of hardlink detection. */
struct seen_inode {
	struct rb_node	i_node;
	u64		i_ino;
	u64		i_subvol;
};
static struct rb_root seen_inodes = RB_ROOT;

static int cmp_si(struct seen_inode *si, u64 ino, u64 subvol)
{
	if (ino < si->i_ino)
		return -1;
	else if (ino > si->i_ino)
		return 1;
	if (subvol < si->i_subvol)
		return -1;
	else if (subvol > si->i_subvol)
		return 1;
	return 0;
}

static int mark_inode_seen(u64 ino, u64 subvol)
{
	int cmp;
	struct rb_node **p = &seen_inodes.rb_node;
	struct rb_node *parent = NULL;
	struct seen_inode *si;

	while (*p) {
		parent = *p;

		si = rb_entry(parent, struct seen_inode, i_node);
		cmp = cmp_si(si, ino, subvol);
		if (cmp < 0)
			p = &(*p)->rb_left;
		else if (cmp > 0)
			p = &(*p)->rb_right;
		else
			BUG();
	}

	si = calloc(1, sizeof(*si));
	if (!si)
		return ENOMEM;

	si->i_ino = ino;
	si->i_subvol = subvol;

	rb_link_node(&si->i_node, parent, p);
	rb_insert_color(&si->i_node, &seen_inodes);

	return 0;
}

static int inode_seen(u64 ino, u64 subvol)
{
	int cmp;
	struct rb_node *n = seen_inodes.rb_node;
	struct seen_inode *si;

	while (n) {
		si = rb_entry(n, struct seen_inode, i_node);

		cmp = cmp_si(si, ino, subvol);
		if (cmp < 0)
			n = n->rb_left;
		else if (cmp > 0)
			n = n->rb_right;
		else
			return EEXIST;
	}
	return 0;

}

const char * const cmd_filesystem_du_usage[] = {
	"btrfs filesystem du [options] <path> [<path>..]",
	"Summarize disk usage of each file.",
	"-h|--human-readable",
	"                   human friendly numbers, base 1024 (default)",
	"-s                 display only a total for each argument",
	NULL
};

/*
 * Inline extents are skipped because they do not take data space,
 * delalloc and unknown are skipped because we do not know how much
 * space they will use yet.
 */
#define	SKIP_FLAGS	(FIEMAP_EXTENT_UNKNOWN|FIEMAP_EXTENT_DELALLOC|FIEMAP_EXTENT_DATA_INLINE)
static int du_calc_file_space(int dirfd, const char *filename,
			      uint64_t *ret_total, uint64_t *ret_shared)
{
	char buf[16384];
	struct fiemap *fiemap = (struct fiemap *)buf;
	struct fiemap_extent *fm_ext = &fiemap->fm_extents[0];
	int count = (sizeof(buf) - sizeof(*fiemap)) /
			sizeof(struct fiemap_extent);
	unsigned int i, ret;
	int last = 0;
	int rc;
	u64 ext_len;
	int fd;
	u64 file_total = 0;
	u64 file_shared = 0;
	u32 flags;

	memset(fiemap, 0, sizeof(struct fiemap));

	fd = openat(dirfd, filename, O_RDONLY);
	if (fd == -1) {
		ret = errno;
		fprintf(stderr, "ERROR: can't access '%s': %s\n",
			filename, strerror(ret));
		return ret;
	}

	do {
		fiemap->fm_length = ~0ULL;
		fiemap->fm_extent_count = count;
		rc = ioctl(fd, FS_IOC_FIEMAP, (unsigned long) fiemap);
		if (rc < 0) {
			ret = errno;
			goto out_close;
		}

		/* If 0 extents are returned, then more ioctls are not needed */
		if (fiemap->fm_mapped_extents == 0)
			break;

		for (i = 0; i < fiemap->fm_mapped_extents; i++) {
			ext_len = fm_ext[i].fe_length;
			flags = fm_ext[i].fe_flags;

			if (flags & FIEMAP_EXTENT_LAST)
				last = 1;

			if (flags & SKIP_FLAGS)
				continue;

			file_total += ext_len;
			if (flags & FIEMAP_EXTENT_SHARED)
				file_shared += ext_len;
		}

		fiemap->fm_start = (fm_ext[i - 1].fe_logical +
				    fm_ext[i - 1].fe_length);
	} while (last == 0);

	*ret_total = file_total;
	*ret_shared = file_shared;

	ret = 0;
out_close:
	close(fd);
	return ret;
}

struct du_dir_ctxt {
	uint64_t	bytes_total;
	uint64_t	bytes_shared;
};

static int du_add_file(const char *filename, int dirfd, uint64_t *ret_total,
		       uint64_t *ret_shared, int top_level);

static int du_walk_dir(struct du_dir_ctxt *ctxt)
{
	int fd, ret, type;
	DIR *dirstream = NULL;
	struct dirent *entry;

	fd = open_file_or_dir(path, &dirstream);
	if (fd < 0)
		return fd;

	ret = 0;
	do {
		uint64_t tot, shr;

		errno = 0;
		entry = readdir(dirstream);
		if (entry) {
			if (strcmp(entry->d_name, ".") == 0
			    || strcmp(entry->d_name, "..") == 0)
				continue;

			type = entry->d_type;
			if (type == DT_REG || type == DT_DIR) {
				tot = shr = 0;

				ret = du_add_file(entry->d_name,
						  dirfd(dirstream), &tot,
						  &shr, 0);
				if (ret)
					break;

				ctxt->bytes_total += tot;
				ctxt->bytes_shared += shr;
			}
		}
	} while (entry != NULL);

	close_file_or_dir(fd, dirstream);
	return ret;
}

static int du_add_file(const char *filename, int dirfd, uint64_t *ret_total,
		       uint64_t *ret_shared, int top_level)
{
	int ret, len = strlen(filename);
	char *pathtmp;
	struct stat st;
	struct du_dir_ctxt dir;
	uint64_t file_total = 0;
	uint64_t file_shared = 0;
	u64 subvol;
	int fd;
	DIR *dirstream = NULL;

	ret = fstatat(dirfd, filename, &st, 0);
	if (ret) {
		ret = errno;
		return ret;
	}

	if (!S_ISREG(st.st_mode) && !S_ISDIR(st.st_mode))
		return 0;

	if (len > (path_max - pathp)) {
		fprintf(stderr, "ERROR: Path max exceeded: %s %s\n", path,
			filename);
		return ENAMETOOLONG;
	}

	pathtmp = pathp;
	if (pathp == path)
		ret = sprintf(pathp, "%s", filename);
	else
		ret = sprintf(pathp, "/%s", filename);
	pathp += ret;

	fd = open_file_or_dir(path, &dirstream);
	if (fd < 0) {
		ret = fd;
		goto out;
	}

	ret = lookup_ino_rootid(fd, &subvol);
	if (ret)
		goto out_close;

	if (inode_seen(st.st_ino, subvol))
		goto out_close;

	ret = mark_inode_seen(st.st_ino, subvol);
	if (ret)
		goto out_close;

	if (S_ISREG(st.st_mode)) {
		ret = du_calc_file_space(dirfd, filename, &file_total,
					 &file_shared);
		if (ret)
			goto out_close;
	} else if (S_ISDIR(st.st_mode)) {
		memset(&dir, 0, sizeof(dir));

		ret = du_walk_dir(&dir);
		*pathp = '\0';
		if (ret)
			goto out_close;

		file_total = dir.bytes_total;
		file_shared = dir.bytes_shared;
	}

	if (!summarize || top_level) {
		printf("%s\t%s\t%s\n", pretty_size_mode(file_total, unit_mode),
		       pretty_size_mode((file_total - file_shared), unit_mode),
		       path);
	}

	if (ret_total)
		*ret_total = file_total;
	if (ret_shared)
		*ret_shared = file_shared;

out_close:
	close_file_or_dir(fd, dirstream);
out:
	/* reset path to just before this element */
	pathp = pathtmp;

	return ret;
}

int cmd_filesystem_du(int argc, char **argv)
{
	int ret = 0, error = 0;
	int i;

	optind = 1;
	while (1) {
		int long_index;
		static const struct option long_options[] = {
			{ "summarize", no_argument, NULL, 's'},
			{ "human-readable", no_argument, NULL, 'h'},
			{ NULL, 0, NULL, 0 }
		};
		int c = getopt_long(argc, argv, "sh", long_options,
				&long_index);

		if (c < 0)
			break;
		switch (c) {
		case 'h':
			unit_mode = UNITS_HUMAN;
			break;
		case 's':
			summarize = 1;
			break;
		default:
			usage(cmd_filesystem_du_usage);
		}
	}

	if (check_argc_min(argc - optind, 1))
		usage(cmd_filesystem_du_usage);

	printf("total\texclusive\tfilename\n");

	for (i = optind; i < argc; i++) {
		ret = du_add_file(argv[i], AT_FDCWD, NULL, NULL, 1);
		if (ret) {
			fprintf(stderr, "ERROR: can't check space of '%s': %s\n",
				argv[i], strerror(ret));
			error = 1;
		}
	}

	return error;
}
