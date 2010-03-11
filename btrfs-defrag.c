/*
 * Copyright (C) 2010 Oracle.  All rights reserved.
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

#ifndef __CHECKER__
#include <sys/ioctl.h>
#include <sys/mount.h>
#include "ioctl.h"
#endif
#include <stdio.h>
#include <stdlib.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <ctype.h>
#include <unistd.h>
#include <dirent.h>
#include <libgen.h>
#include <getopt.h>
#include "kerncompat.h"
#include "ctree.h"
#include "transaction.h"
#include "utils.h"
#include "version.h"

static u64 parse_size(char *s)
{
	int len = strlen(s);
	char c;
	u64 mult = 1;

	if (!isdigit(s[len - 1])) {
		c = tolower(s[len - 1]);
		switch (c) {
		case 'g':
			mult *= 1024;
		case 'm':
			mult *= 1024;
		case 'k':
			mult *= 1024;
		case 'b':
			break;
		default:
			fprintf(stderr, "Unknown size descriptor %c\n", c);
			exit(1);
		}
		s[len - 1] = '\0';
	}
	return atoll(s) * mult;
}

static void print_usage(void)
{
	printf("usage: btrfs-defrag [-c] [-f] [-s start] [-l len] "
	       "[-t threshold] file ...\n");
	exit(1);
}

int main(int ac, char **av)
{
	int fd;
	int compress = 0;
	int flush = 0;
	u64 start = 0;
	u64 len = (u64)-1;
	u32 thresh = 0;
	int i;
	int errors = 0;
	int ret = 0;
	int verbose = 0;
	struct btrfs_ioctl_defrag_range_args range;

	while(1) {
		int c = getopt(ac, av, "vcfs:l:t:");
		if (c < 0)
			break;
		switch(c) {
		case 'c':
			compress = 1;
			break;
		case 'f':
			flush = 1;
			break;
		case 'v':
			verbose = 1;
			break;
		case 's':
			start = parse_size(optarg);
			break;
		case 'l':
			len = parse_size(optarg);
			break;
		case 't':
			thresh = parse_size(optarg);
			break;
		default:
			print_usage();
			return 1;
		}
	}
	if (ac - optind == 0)
		print_usage();

	memset(&range, 0, sizeof(range));
	range.start = start;
	range.len = len;
	range.extent_thresh = thresh;
	if (compress)
		range.flags |= BTRFS_DEFRAG_RANGE_COMPRESS;
	if (flush)
		range.flags |= BTRFS_DEFRAG_RANGE_START_IO;

	for (i = optind; i < ac; i++) {
		fd = open(av[i], O_RDWR);
		if (fd < 0) {
			fprintf(stderr, "failed to open %s\n", av[i]);
			perror("open:");
			errors++;
			continue;
		}
		ret = ioctl(fd, BTRFS_IOC_DEFRAG_RANGE, &range);
		if (ret) {
			fprintf(stderr, "ioctl failed on %s ret %d\n",
				av[i], ret);
			errors++;
		}
		close(fd);
	}
	if (verbose)
		printf("%s\n", BTRFS_BUILD_VERSION);
	if (errors) {
		fprintf(stderr, "total %d failures\n", errors);
		exit(1);
	}
	return 0;
}

