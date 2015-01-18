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
#include <sys/ioctl.h>
#include <sys/types.h>
#include <dirent.h>
#include <sys/stat.h>
#include <unistd.h>
#include <fcntl.h>
#include <libgen.h>
#include <limits.h>
#include <uuid/uuid.h>
#include <ctype.h>

#include <gd.h>

#undef ULONG_MAX

#include "kerncompat.h"
#include "ctree.h"
#include "ioctl.h"
#include "utils.h"

static int use_color;
static void
push_im(gdImagePtr im, char *name, char *dir)
{
	char fullname[2000];
	FILE *pngout;

	if (!im)
		return;

	snprintf(fullname, sizeof(fullname), "%s/%s", dir, name);
	pngout = fopen(fullname, "w");
	if (!pngout) {
		printf("unable to create file %s\n", fullname);
		exit(1);
	}

	gdImagePng(im, pngout);

	fclose(pngout);
	gdImageDestroy(im);
}

static char *
chunk_type(u64 flags)
{
	switch (flags & (BTRFS_BLOCK_GROUP_SYSTEM | BTRFS_BLOCK_GROUP_DATA |
			 BTRFS_BLOCK_GROUP_METADATA)) {
	case BTRFS_BLOCK_GROUP_SYSTEM:
		return "system";
	case BTRFS_BLOCK_GROUP_DATA:
		return "data";
	case BTRFS_BLOCK_GROUP_METADATA:
		return "metadata";
	case BTRFS_BLOCK_GROUP_DATA | BTRFS_BLOCK_GROUP_METADATA:
		return "mixed";
	default:
		return "invalid";
	}
}

static void
print_bg(FILE *html, char *name, u64 start, u64 len, u64 used, u64 flags,
	 u64 areas)
{
	double frag = (double)areas / (len / 4096) * 2;

	fprintf(html, "<p>%s chunk starts at %lld, size is %s, %.2f%% used, "
		      "%.2f%% fragmented</p>\n", chunk_type(flags), start,
		      pretty_size(len), 100.0 * used / len, 100.0 * frag);
	fprintf(html, "<img src=\"%s\" border=\"1\" />\n", name);
}

enum tree_colors {
	COLOR_ROOT = 0,
	COLOR_EXTENT,
	COLOR_CHUNK,
	COLOR_DEV,
	COLOR_FS,
	COLOR_CSUM,
	COLOR_RELOC,
	COLOR_DATA,
	COLOR_UNKNOWN,
	COLOR_MAX
};

static int
get_color(struct btrfs_extent_item *item, int len)
{
	u64 refs;
	u64 flags;
	u8 type;
	u64 offset;
	struct btrfs_extent_inline_ref *ref;

	refs = btrfs_stack_extent_refs(item);
	flags = btrfs_stack_extent_flags(item);

	if (flags & BTRFS_EXTENT_FLAG_DATA)
		return COLOR_DATA;
	if (refs > 1) {
		/* this must be an fs tree */
		return COLOR_FS;
	}

	ref = (void *)item + sizeof(struct btrfs_extent_item) +
			     sizeof(struct btrfs_tree_block_info);
	type = btrfs_stack_extent_inline_ref_type(ref);
	offset = btrfs_stack_extent_inline_ref_offset(ref);

	switch (type) {
	case BTRFS_EXTENT_DATA_REF_KEY:
		return COLOR_DATA;
	case BTRFS_SHARED_BLOCK_REF_KEY:
	case BTRFS_SHARED_DATA_REF_KEY:
		return COLOR_FS;
	case BTRFS_TREE_BLOCK_REF_KEY:
		break;
	default:
		return COLOR_UNKNOWN;
	}

	switch (offset) {
	case BTRFS_ROOT_TREE_OBJECTID:
		return COLOR_ROOT;
	case BTRFS_EXTENT_TREE_OBJECTID:
		return COLOR_EXTENT;
	case BTRFS_CHUNK_TREE_OBJECTID:
		return COLOR_CHUNK;
	case BTRFS_DEV_TREE_OBJECTID:
		return COLOR_DEV;
	case BTRFS_FS_TREE_OBJECTID:
		return COLOR_FS;
	case BTRFS_CSUM_TREE_OBJECTID:
		return COLOR_CSUM;
	case BTRFS_DATA_RELOC_TREE_OBJECTID:
		return COLOR_RELOC;
	}

	return COLOR_UNKNOWN;
}

static void
init_colors(gdImagePtr im, int *colors)
{
	colors[COLOR_ROOT] = gdImageColorAllocate(im, 255, 0, 0);
	colors[COLOR_EXTENT] = gdImageColorAllocate(im, 0, 255, 0);
	colors[COLOR_CHUNK] = gdImageColorAllocate(im, 255, 0, 0);
	colors[COLOR_DEV] = gdImageColorAllocate(im, 255, 0, 0);
	colors[COLOR_FS] = gdImageColorAllocate(im, 0, 0, 0);
	colors[COLOR_CSUM] = gdImageColorAllocate(im, 0, 0, 255);
	colors[COLOR_RELOC] = gdImageColorAllocate(im, 128, 128, 128);
	colors[COLOR_DATA] = gdImageColorAllocate(im, 100, 0, 0);
	colors[COLOR_UNKNOWN] = gdImageColorAllocate(im, 50, 50, 50);
}

int
list_fragments(int fd, u64 flags, char *dir)
{
	int ret;
	struct btrfs_ioctl_search_args args;
	struct btrfs_ioctl_search_key *sk = &args.key;
	int i;
	struct btrfs_ioctl_search_header *sh;
	unsigned long off = 0;
	int bgnum = 0;
	u64 bgstart = 0;
	u64 bglen = 0;
	u64 bgend = 0;
	u64 bgflags = 0;
	u64 bgused = 0;
	u64 saved_extent = 0;
	u64 saved_len = 0;
	int saved_color = 0;
	u64 last_end = 0;
	u64 areas = 0;
	long px;
	char name[1000];
	FILE *html;
	int colors[COLOR_MAX];

	gdImagePtr im = NULL;
	int black = 0;
	int width = 800;

	snprintf(name, sizeof(name), "%s/index.html", dir);
	html = fopen(name, "w");
	if (!html) {
		printf("unable to create %s\n", name);
		exit(1);
	}

	fprintf(html, "<html><header>\n");
	fprintf(html, "<title>Btrfs Block Group Allocation Map</title>\n");
	fprintf(html, "<style type=\"text/css\">\n");
	fprintf(html, "img {margin-left: 1em; margin-bottom: 2em;}\n");
	fprintf(html, "</style>\n");
	fprintf(html, "</header><body>\n");
	
	memset(&args, 0, sizeof(args));

	sk->tree_id = 2;
	sk->max_type = -1;
	sk->min_type = 0;
	sk->max_objectid = (u64)-1;
	sk->max_offset = (u64)-1;
	sk->max_transid = (u64)-1;

	/* just a big number, doesn't matter much */
	sk->nr_items = 4096;

	while(1) {
		ret = ioctl(fd, BTRFS_IOC_TREE_SEARCH, &args);
		if (ret < 0) {
			fprintf(stderr, "ERROR: can't perform the search\n");
			goto out_close;
		}
		/* the ioctl returns the number of item it found in nr_items */
		if (sk->nr_items == 0)
			break;

		off = 0;
		for (i = 0; i < sk->nr_items; i++) {
			int j;

			sh = (struct btrfs_ioctl_search_header *)(args.buf +
								  off);
			off += sizeof(*sh);
			if (sh->type == BTRFS_BLOCK_GROUP_ITEM_KEY) {
				struct btrfs_block_group_item *bg;

				if (im) {
					push_im(im, name, dir);
					im = NULL;

					print_bg(html, name, bgstart, bglen,
						bgused, bgflags, areas);
				}

				++bgnum;

				bg = (struct btrfs_block_group_item *)
						(args.buf + off);
				bgflags = btrfs_block_group_flags(bg);
				bgused = btrfs_block_group_used(bg);
				
				printf("found block group %lld len %lld "
					"flags %lld\n", sh->objectid,
					sh->offset, bgflags);
				if (!(bgflags & flags)) {
					/* skip this block group */
					sk->min_objectid = sh->objectid +
							   sh->offset;
					sk->min_type = 0;
					sk->min_offset = 0;
					break;
				}
				im = gdImageCreate(width,
					(sh->offset / 4096 + 799) / width);

				black = gdImageColorAllocate(im, 0, 0, 0);  

				for (j = 0; j < ARRAY_SIZE(colors); ++j)
					colors[j] = black;

				init_colors(im, colors);
				bgstart = sh->objectid;
				bglen = sh->offset;
				bgend = bgstart + bglen;

				snprintf(name, sizeof(name), "bg%d.png", bgnum);

				last_end = bgstart;
				if (saved_len) {
					px = (saved_extent - bgstart) / 4096;
					for (j = 0; j < saved_len / 4096; ++j) {
						int x = (px + j) % width;
						int y = (px + j) / width;
						gdImageSetPixel(im, x, y,
								saved_color);
					}
					last_end += saved_len;
				}
				areas = 0;
				saved_len = 0;
			}
			if (im && sh->type == BTRFS_EXTENT_ITEM_KEY) {
				int c;
				struct btrfs_extent_item *item;

				item = (struct btrfs_extent_item *)
						(args.buf + off);

				if (use_color)
					c = colors[get_color(item, sh->len)];
				else
					c = black;
				if (sh->objectid > bgend) {
					printf("WARN: extent %lld is without "
						"block group\n", sh->objectid);
					goto skip;
				}
				if (sh->objectid == bgend) {
					saved_extent = sh->objectid;
					saved_len = sh->offset;
					saved_color = c;
					goto skip;
				}
				px = (sh->objectid - bgstart) / 4096;
				for (j = 0; j < sh->offset / 4096; ++j) {
					int x = (px + j) % width;
					int y = (px + j) / width;
					gdImageSetPixel(im, x, y, c);
				}
				if (sh->objectid != last_end)
					++areas;
				last_end = sh->objectid + sh->offset;
skip:;
			}
			off += sh->len;

			/*
			 * record the mins in sk so we can make sure the
			 * next search doesn't repeat this root
			 */
			sk->min_objectid = sh->objectid;
			sk->min_type = sh->type;
			sk->min_offset = sh->offset;
		}
		sk->nr_items = 4096;

		/* increment by one */
		if (++sk->min_offset == 0)
			if (++sk->min_type == 0)
				if (++sk->min_objectid == 0)
					break;
	}

	if (im) {
		push_im(im, name, dir);
		print_bg(html, name, bgstart, bglen, bgused, bgflags, areas);
	}

	if (use_color) {
		fprintf(html, "<p>");
		fprintf(html, "data - dark red, ");
		fprintf(html, "fs tree - black, ");
		fprintf(html, "extent tree - green, ");
		fprintf(html, "csum tree - blue, ");
		fprintf(html, "reloc tree - grey, ");
		fprintf(html, "other trees - red, ");
		fprintf(html, "unknown tree - dark grey");
		fprintf(html, "</p>");
	}
	fprintf(html, "</body></html>\n");

out_close:
	fclose(html);

	return ret;
}

void
usage(void)
{
	printf("usage: btrfs-fragments [options] <path>\n");
	printf("         -c               use color\n");
	printf("         -d               print data chunks\n");
	printf("         -m               print metadata chunks\n");
	printf("         -s               print system chunks\n");
	printf("                          (default is data+metadata)\n");
	printf("         -o <dir>         output directory, default is html\n");
	exit(1);
}

int main(int argc, char **argv)
{
	char *path;
	int fd;
	int ret;
	u64 flags = 0;
	char *dir = "html";
	DIR *dirstream = NULL;

	while (1) {
		int c = getopt(argc, argv, "cmso:h");
		if (c < 0)
			break;
		switch (c) {
		case 'c':
			use_color = 1;
			break;
		case 'd':
			flags |= BTRFS_BLOCK_GROUP_DATA;
			break;
		case 'm':
			flags |= BTRFS_BLOCK_GROUP_METADATA;
			break;
		case 's':
			flags |= BTRFS_BLOCK_GROUP_SYSTEM;
			break;
		case 'o':
			dir = optarg;
			break;
		case 'h':
		default:
			usage();
		}
	}

	set_argv0(argv);
	argc = argc - optind;
	if (check_argc_min(argc, 1)) {
		usage();
		exit(1);
	}

	path = argv[optind++];

	fd = open_file_or_dir(path, &dirstream);
	if (fd < 0) {
		fprintf(stderr, "ERROR: can't access '%s'\n", path);
		exit(1);
	}

	if (flags == 0)
		flags = BTRFS_BLOCK_GROUP_DATA | BTRFS_BLOCK_GROUP_METADATA;

	ret = list_fragments(fd, flags, dir);
	close_file_or_dir(fd, dirstream);
	if (ret)
		exit(1);

	exit(0);
}
