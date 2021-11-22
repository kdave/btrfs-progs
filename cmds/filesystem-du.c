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
#include <linux/version.h>
#include <linux/fiemap.h>

#if !defined(FIEMAP_EXTENT_SHARED) && (HAVE_OWN_FIEMAP_EXTENT_SHARED_DEFINE == 1)
#define FIEMAP_EXTENT_SHARED           0x00002000
#endif

#include "common/utils.h"
#include "cmds/commands.h"
#include "kerncompat.h"
#include "kernel-lib/rbtree.h"
#include "kernel-lib/interval_tree_generic.h"
#include "common/open-utils.h"
#include "common/units.h"
#include "common/help.h"
#include "common/fsfeatures.h"

static int summarize = 0;
static unsigned unit_mode = UNITS_RAW;
static char path[PATH_MAX] = { 0, };
static char *pathp = path;
static char *path_max = &path[PATH_MAX - 1];

struct shared_extent {
	struct rb_node	rb;
	u64	start;	/* Start of interval */
	u64	last;	/* Last location _in_ interval */
	u64	__subtree_last;
};

/*
 * extent_tree_* functions are defined in the massive interval tree
 * macro below. This serves to illustrate the api in human-readable
 * terms.
 */
static void
extent_tree_insert(struct shared_extent *node, struct rb_root *root);

static void
extent_tree_remove(struct shared_extent *node, struct rb_root *root);

static struct shared_extent *
extent_tree_iter_first(struct rb_root *root,
		       u64 start, u64 last);

static struct shared_extent *
extent_tree_iter_next(struct shared_extent *node,
			u64 start, u64 last);

#define START(node) ((node)->start)
#define LAST(node)  ((node)->last)

INTERVAL_TREE_DEFINE(struct shared_extent, rb,
		     u64, __subtree_last,
		     START, LAST, static, extent_tree)

static int add_shared_extent(u64 start, u64 len, struct rb_root *root)
{
	struct shared_extent *sh;

	ASSERT(len != 0);

	sh = calloc(1, sizeof(*sh));
	if (!sh)
		return -ENOMEM;

	sh->start = start;
	sh->last = (start + len - 1);

	extent_tree_insert(sh, root);

	return 0;
}

static void cleanup_shared_extents(struct rb_root *root)
{
	struct shared_extent *s;
	struct shared_extent *tmp;

	if (!root)
		return;

	s = extent_tree_iter_first(root, 0, -1ULL);
	while (s) {
		tmp = extent_tree_iter_next(s, 0, -1ULL);
		extent_tree_remove(s, root);

		free(s);
		s = tmp;
	}
}

#define dbgprintf(...)

/*
 * Find all extents which overlap 'n', calculate the space
 * covered by them and remove those nodes from the tree.
 */
static u64 count_unique_bytes(struct rb_root *root, struct shared_extent *n)
{
	struct shared_extent *tmp;
	u64 wstart = n->start;
	u64 wlast = n->last;

	dbgprintf("Count overlaps:");

	do {
		/*
		 * Expand our search window based on the latest
		 * overlapping extent. Doing this will allow us to
		 * find all possible overlaps
		 */
		if (wstart > n->start)
			wstart = n->start;
		if (wlast < n->last)
			wlast = n->last;

		dbgprintf(" (%llu, %llu)", n->start, n->last);

		tmp = n;
		n = extent_tree_iter_next(n, wstart, wlast);

		extent_tree_remove(tmp, root);
		free(tmp);
	} while (n);

	dbgprintf("; wstart: %llu wlast: %llu total: %llu\n", wstart,
		wlast, wlast - wstart + 1);

	return wlast - wstart + 1;
}

/*
 * What we want to do here is get a count of shared bytes within the
 * set of extents we have collected. Specifically, we don't want to
 * count any byte more than once, so just adding them up doesn't
 * work.
 *
 * For each set of overlapping extents we find the lowest start and
 * highest end. From there we have the actual number of bytes which is
 * shared across all of the extents in our set. A sum of each sets
 * extent length is returned.
 */
static void count_shared_bytes(struct rb_root *root, u64 *ret_cnt)
{
	u64 count = 0;
	struct shared_extent *s = extent_tree_iter_first(root,
							 0, -1ULL);

	if (!s)
		goto out;

	while (s) {
		/*
		 * Find all extents which overlap 's', calculate the space
		 * covered by them and remove those nodes from the tree.
		 */
		count += count_unique_bytes(root, s);

		/*
		 * Since count_unique_bytes will be emptying the tree,
		 * we can grab the first node here
		 */
		s = extent_tree_iter_first(root, 0, -1ULL);
	}

	BUG_ON(!RB_EMPTY_ROOT(root));
out:
	*ret_cnt = count;
}

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
			return -EEXIST;
	}

	si = calloc(1, sizeof(*si));
	if (!si)
		return -ENOMEM;

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
			return -EEXIST;
	}
	return 0;
}

static void clear_seen_inodes(void)
{
	struct rb_node *n = rb_first(&seen_inodes);
	struct seen_inode *si;

	while (n) {
		si = rb_entry(n, struct seen_inode, i_node);

		rb_erase(&si->i_node, &seen_inodes);
		free(si);

		n = rb_first(&seen_inodes);
	}
}

/*
 * Inline extents are skipped because they do not take data space,
 * delalloc and unknown are skipped because we do not know how much
 * space they will use yet.
 */
#define	SKIP_FLAGS	(FIEMAP_EXTENT_UNKNOWN|FIEMAP_EXTENT_DELALLOC|FIEMAP_EXTENT_DATA_INLINE)
static int du_calc_file_space(int fd, struct rb_root *shared_extents,
			      u64 *ret_total, u64 *ret_shared)
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
	u64 file_total = 0;
	u64 file_shared = 0;
	u32 flags;

	memset(fiemap, 0, sizeof(struct fiemap));

	do {
		fiemap->fm_length = ~0ULL;
		fiemap->fm_extent_count = count;
		rc = ioctl(fd, FS_IOC_FIEMAP, (unsigned long) fiemap);
		if (rc < 0) {
			ret = -errno;
			goto out;
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

			if (ext_len == 0) {
				warning("extent %llu has length 0, skipping",
					(unsigned long long)fm_ext[i].fe_physical);
				continue;
			}

			file_total += ext_len;
			if (flags & FIEMAP_EXTENT_SHARED) {
				file_shared += ext_len;

				if (shared_extents) {
					ret = add_shared_extent(fm_ext[i].fe_physical,
								ext_len,
								shared_extents);
					if (ret)
						goto out;
				}
			}
		}

		fiemap->fm_start = (fm_ext[i - 1].fe_logical +
				    fm_ext[i - 1].fe_length);
	} while (last == 0);

	*ret_total = file_total;
	*ret_shared = file_shared;

	ret = 0;
out:
	return ret;
}

struct du_dir_ctxt {
	u64		bytes_total;
	u64		bytes_shared;
	DIR		*dirstream;
	struct rb_root	shared_extents;
};
#define INIT_DU_DIR_CTXT	(struct du_dir_ctxt) { 0ULL, 0ULL, NULL, RB_ROOT }

static int du_add_file(const char *filename, int dirfd,
		       struct rb_root *shared_extents, u64 *ret_total,
		       u64 *ret_shared, int top_level);

static int du_walk_dir(struct du_dir_ctxt *ctxt, struct rb_root *shared_extents)
{
	int ret, type;
	struct dirent *entry;
	DIR *dirstream = ctxt->dirstream;

	ret = 0;
	do {
		u64 tot, shr;

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
						  dirfd(dirstream),
						  shared_extents, &tot, &shr,
						  0);
				if (ret) {
					errno = -ret;
					fprintf(stderr, "cannot access: '%s:' %m\n",
							entry->d_name);
					if (ret == -ENOTTY || ret == -EACCES) {
						ret = 0;
						continue;
					}
					break;
				}

				ctxt->bytes_total += tot;
				ctxt->bytes_shared += shr;
			}
		}
	} while (entry != NULL);

	return ret;
}

static int du_add_file(const char *filename, int dirfd,
		       struct rb_root *shared_extents, u64 *ret_total,
		       u64 *ret_shared, int top_level)
{
	int ret, len = strlen(filename);
	char *pathtmp;
	struct stat st;
	struct du_dir_ctxt dir = INIT_DU_DIR_CTXT;
	int is_dir = 0;
	u64 file_total = 0;
	u64 file_shared = 0;
	u64 dir_set_shared = 0;
	int fd;
	DIR *dirstream = NULL;

	ret = fstatat(dirfd, filename, &st, 0);
	if (ret)
		return -errno;

	if (!S_ISREG(st.st_mode) && !S_ISDIR(st.st_mode))
		return 0;

	if (len > (path_max - pathp)) {
		error("path too long: %s %s", path, filename);
		return -ENAMETOOLONG;
	}

	pathtmp = pathp;
	if (pathp == path || *(pathp - 1) == '/')
		ret = sprintf(pathp, "%s", filename);
	else
		ret = sprintf(pathp, "/%s", filename);
	pathp += ret;

	fd = open_file_or_dir3(path, &dirstream, O_RDONLY);
	if (fd < 0) {
		ret = -errno;
		goto out;
	}

	/*
	 * If st.st_ino == BTRFS_EMPTY_SUBVOL_DIR_OBJECTID ==2, there is no any
	 * related tree
	 */
	if (st.st_ino != BTRFS_EMPTY_SUBVOL_DIR_OBJECTID) {
		u64 subvol;

		ret = lookup_path_rootid(fd, &subvol);
		if (ret)
			goto out_close;

		if (inode_seen(st.st_ino, subvol))
			goto out_close;

		ret = mark_inode_seen(st.st_ino, subvol);
		if (ret)
			goto out_close;
	}

	if (S_ISREG(st.st_mode)) {
		ret = du_calc_file_space(fd, shared_extents, &file_total,
					 &file_shared);
		if (ret)
			goto out_close;
	} else if (S_ISDIR(st.st_mode)) {
		struct rb_root *root = shared_extents;

		/*
		 * We collect shared extents in an rb_root, the top
		 * level caller will not pass a root down, so use the
		 * one on our dir context.
		 */
		if (top_level)
			root = &dir.shared_extents;

		is_dir = 1;

		dir.dirstream = dirstream;
		ret = du_walk_dir(&dir, root);
		*pathp = '\0';
		if (ret) {
			if (top_level)
				cleanup_shared_extents(root);
			goto out_close;
		}

		file_total = dir.bytes_total;
		file_shared = dir.bytes_shared;
		if (top_level)
			count_shared_bytes(root, &dir_set_shared);
	}

	if (!summarize || top_level) {
		u64 excl = file_total - file_shared;

		if (top_level) {
			u64 set_shared = file_shared;

			if (is_dir)
				set_shared = dir_set_shared;

			printf("%10s  %10s  %10s  %s\n",
			       pretty_size_mode(file_total, unit_mode),
			       pretty_size_mode(excl, unit_mode),
			       pretty_size_mode(set_shared, unit_mode),
			       path);
		} else {
			printf("%10s  %10s  %10s  %s\n",
			       pretty_size_mode(file_total, unit_mode),
			       pretty_size_mode(excl, unit_mode),
			       "-", path);
		}
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

static const char * const cmd_filesystem_du_usage[] = {
	"btrfs filesystem du [options] <path> [<path>..]",
	"Summarize disk usage of each file.",
	"",
	"-s|--summarize     display only a total for each argument",
	HELPINFO_UNITS_LONG,
	NULL
};

static int cmd_filesystem_du(const struct cmd_struct *cmd,
			     int argc, char **argv)
{
	int ret = 0, err = 0;
	int i;
	u32 kernel_version;

	unit_mode = get_unit_mode_from_arg(&argc, argv, 0);

	optind = 0;
	while (1) {
		static const struct option long_options[] = {
			{ "summarize", no_argument, NULL, 's'},
			{ NULL, 0, NULL, 0 }
		};
		int c = getopt_long(argc, argv, "s", long_options, NULL);

		if (c < 0)
			break;
		switch (c) {
		case 's':
			summarize = 1;
			break;
		default:
			usage_unknown_option(cmd, argv);
		}
	}

	if (check_argc_min(argc - optind, 1))
		return 1;

	kernel_version = get_running_kernel_version();

	if (kernel_version < KERNEL_VERSION(2,6,33)) {
		warning(
"old kernel version detected, shared space will be reported as exclusive\n"
"due to missing support for FIEMAP_EXTENT_SHARED flag");
	}

	printf("%10s  %10s  %10s  %s\n", "Total", "Exclusive", "Set shared",
			"Filename");

	for (i = optind; i < argc; i++) {
		ret = du_add_file(argv[i], AT_FDCWD, NULL, NULL, NULL, 1);
		if (ret) {
			errno = -ret;
			error("cannot check space of '%s': %m", argv[i]);
			err = 1;
		}

		/* reset hard-link detection for each argument */
		clear_seen_inodes();
	}

	return err;
}
DEFINE_SIMPLE_COMMAND(filesystem_du, "du");
