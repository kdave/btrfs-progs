/*
 * Copyright (C) 2012 STRATO AG.  All rights reserved.
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

#define _XOPEN_SOURCE 500
#define _GNU_SOURCE 1
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <fcntl.h>
#include <sys/stat.h>
#include <ctype.h>
#include <uuid/uuid.h>
#include <errno.h>

#include "kerncompat.h"
#include "ctree.h"
#include "disk-io.h"
#include "print-tree.h"
#include "transaction.h"
#include "list.h"
#include "version.h"
#include "utils.h"
#include "crc32c.h"

static void print_usage(void);
static void dump_superblock(struct btrfs_super_block *sb);
int main(int argc, char **argv);
static int load_and_dump_sb(char *, int fd, u64 sb_bytenr);


static void print_usage(void)
{
	fprintf(stderr,
		"usage: btrfs-show-super [-i super_mirror|-a] dev [dev..]\n");
	fprintf(stderr, "\tThe super_mirror number is between 0 and %d.\n",
		BTRFS_SUPER_MIRROR_MAX - 1);
	fprintf(stderr, "\tIf -a is passed all the superblocks are showed.\n");
	fprintf(stderr, "%s\n", BTRFS_BUILD_VERSION);
}

int main(int argc, char **argv)
{
	int opt;
	int all = 0;
	char *filename;
	int fd = -1;
	int arg, i;
	u64 sb_bytenr = btrfs_sb_offset(0);

	while ((opt = getopt(argc, argv, "ai:")) != -1) {
		switch (opt) {
		case 'i':
			arg = atoi(optarg);

			if (arg < 0 || arg >= BTRFS_SUPER_MIRROR_MAX) {
				fprintf(stderr,
					"Illegal super_mirror %d\n",
					arg);
				print_usage();
				exit(1);
			}
			sb_bytenr = btrfs_sb_offset(arg);
			break;

		case 'a':
			all = 1;
			break;

		default:
			print_usage();
			exit(1);
		}
	}

	if (argc < optind + 1) {
		print_usage();
		exit(1);
	}

	for (i = optind; i < argc; i++) {
		filename = argv[i];
		fd = open(filename, O_RDONLY, 0666);
		if (fd < 0) {
			fprintf(stderr, "Could not open %s\n", filename);
			exit(1);
		}

		if (all) {
			int idx;
			for (idx = 0; idx < BTRFS_SUPER_MIRROR_MAX; idx++) {
				sb_bytenr = btrfs_sb_offset(idx);
				if (load_and_dump_sb(filename, fd, sb_bytenr)) {
					close(fd);
					exit(1);
				}

				putchar('\n');
			}
		} else {
			load_and_dump_sb(filename, fd, sb_bytenr);
			putchar('\n');
		}
		close(fd);
	}

	exit(0);
}

static int load_and_dump_sb(char *filename, int fd, u64 sb_bytenr)
{
	u8 super_block_data[BTRFS_SUPER_INFO_SIZE];
	struct btrfs_super_block *sb;
	u64 ret;

	sb = (struct btrfs_super_block *)super_block_data;

	ret = pread64(fd, super_block_data, BTRFS_SUPER_INFO_SIZE, sb_bytenr);
	if (ret != BTRFS_SUPER_INFO_SIZE) {
		int e = errno;

		/* check if the disk if too short for further superblock */
		if (ret == 0 && e == 0)
			return 0;

		fprintf(stderr,
		   "ERROR: Failed to read the superblock on %s at %llu\n",
		   filename, (unsigned long long)sb_bytenr);
		fprintf(stderr,
		   "ERROR: error = '%s', errno = %d\n", strerror(e), e);
		return 1;
	}
	printf("superblock: bytenr=%llu, device=%s\n", sb_bytenr, filename);
	printf("---------------------------------------------------------\n");
	dump_superblock(sb);
	return 0;
}

static int check_csum_sblock(void *sb, int csum_size)
{
	char result[BTRFS_CSUM_SIZE];
	u32 crc = ~(u32)0;

	crc = btrfs_csum_data(NULL, (char *)sb + BTRFS_CSUM_SIZE,
				crc, BTRFS_SUPER_INFO_SIZE - BTRFS_CSUM_SIZE);
	btrfs_csum_final(crc, result);

	return !memcmp(sb, &result, csum_size);
}

static void dump_superblock(struct btrfs_super_block *sb)
{
	int i;
	char *s, buf[BTRFS_UUID_UNPARSED_SIZE];
	u8 *p;

	printf("csum\t\t\t0x");
	for (i = 0, p = sb->csum; i < btrfs_super_csum_size(sb); i++)
		printf("%02x", p[i]);
	if (check_csum_sblock(sb, btrfs_super_csum_size(sb)))
		printf(" [match]");
	else
		printf(" [DON'T MATCH]");
	putchar('\n');

	printf("bytenr\t\t\t%llu\n",
		(unsigned long long)btrfs_super_bytenr(sb));
	printf("flags\t\t\t0x%llx\n",
		(unsigned long long)btrfs_super_flags(sb));

	printf("magic\t\t\t");
	s = (char *) &sb->magic;
	for (i = 0; i < 8; i++)
		putchar(isprint(s[i]) ? s[i] : '.');
	if (btrfs_super_magic(sb) == BTRFS_MAGIC)
		printf(" [match]\n");
	else
		printf(" [DON'T MATCH]\n");

	uuid_unparse(sb->fsid, buf);
	printf("fsid\t\t\t%s\n", buf);

	printf("label\t\t\t");
	s = sb->label;
	for (i = 0; i < BTRFS_LABEL_SIZE && s[i]; i++)
		putchar(isprint(s[i]) ? s[i] : '.');
	putchar('\n');

	printf("generation\t\t%llu\n",
	       (unsigned long long)btrfs_super_generation(sb));
	printf("root\t\t\t%llu\n", (unsigned long long)btrfs_super_root(sb));
	printf("sys_array_size\t\t%llu\n",
	       (unsigned long long)btrfs_super_sys_array_size(sb));
	printf("chunk_root_generation\t%llu\n",
	       (unsigned long long)btrfs_super_chunk_root_generation(sb));
	printf("root_level\t\t%llu\n",
	       (unsigned long long)btrfs_super_root_level(sb));
	printf("chunk_root\t\t%llu\n",
	       (unsigned long long)btrfs_super_chunk_root(sb));
	printf("chunk_root_level\t%llu\n",
	       (unsigned long long)btrfs_super_chunk_root_level(sb));
	printf("log_root\t\t%llu\n",
	       (unsigned long long)btrfs_super_log_root(sb));
	printf("log_root_transid\t%llu\n",
	       (unsigned long long)btrfs_super_log_root_transid(sb));
	printf("log_root_level\t\t%llu\n",
	       (unsigned long long)btrfs_super_log_root_level(sb));
	printf("total_bytes\t\t%llu\n",
	       (unsigned long long)btrfs_super_total_bytes(sb));
	printf("bytes_used\t\t%llu\n",
	       (unsigned long long)btrfs_super_bytes_used(sb));
	printf("sectorsize\t\t%llu\n",
	       (unsigned long long)btrfs_super_sectorsize(sb));
	printf("nodesize\t\t%llu\n",
	       (unsigned long long)btrfs_super_nodesize(sb));
	printf("leafsize\t\t%llu\n",
	       (unsigned long long)btrfs_super_leafsize(sb));
	printf("stripesize\t\t%llu\n",
	       (unsigned long long)btrfs_super_stripesize(sb));
	printf("root_dir\t\t%llu\n",
	       (unsigned long long)btrfs_super_root_dir(sb));
	printf("num_devices\t\t%llu\n",
	       (unsigned long long)btrfs_super_num_devices(sb));
	printf("compat_flags\t\t0x%llx\n",
	       (unsigned long long)btrfs_super_compat_flags(sb));
	printf("compat_ro_flags\t\t0x%llx\n",
	       (unsigned long long)btrfs_super_compat_ro_flags(sb));
	printf("incompat_flags\t\t0x%llx\n",
	       (unsigned long long)btrfs_super_incompat_flags(sb));
	printf("csum_type\t\t%llu\n",
	       (unsigned long long)btrfs_super_csum_type(sb));
	printf("csum_size\t\t%llu\n",
	       (unsigned long long)btrfs_super_csum_size(sb));
	printf("cache_generation\t%llu\n",
	       (unsigned long long)btrfs_super_cache_generation(sb));
	printf("uuid_tree_generation\t%llu\n",
	       (unsigned long long)btrfs_super_uuid_tree_generation(sb));

	uuid_unparse(sb->dev_item.uuid, buf);
	printf("dev_item.uuid\t\t%s\n", buf);

	uuid_unparse(sb->dev_item.fsid, buf);
	printf("dev_item.fsid\t\t%s %s\n", buf,
		!memcmp(sb->dev_item.fsid, sb->fsid, BTRFS_FSID_SIZE) ?
			"[match]" : "[DON'T MATCH]");

	printf("dev_item.type\t\t%llu\n", (unsigned long long)
	       btrfs_stack_device_type(&sb->dev_item));
	printf("dev_item.total_bytes\t%llu\n", (unsigned long long)
	       btrfs_stack_device_total_bytes(&sb->dev_item));
	printf("dev_item.bytes_used\t%llu\n", (unsigned long long)
	       btrfs_stack_device_bytes_used(&sb->dev_item));
	printf("dev_item.io_align\t%u\n", (unsigned int)
	       btrfs_stack_device_io_align(&sb->dev_item));
	printf("dev_item.io_width\t%u\n", (unsigned int)
	       btrfs_stack_device_io_width(&sb->dev_item));
	printf("dev_item.sector_size\t%u\n", (unsigned int)
	       btrfs_stack_device_sector_size(&sb->dev_item));
	printf("dev_item.devid\t\t%llu\n",
	       btrfs_stack_device_id(&sb->dev_item));
	printf("dev_item.dev_group\t%u\n", (unsigned int)
	       btrfs_stack_device_group(&sb->dev_item));
	printf("dev_item.seek_speed\t%u\n", (unsigned int)
	       btrfs_stack_device_seek_speed(&sb->dev_item));
	printf("dev_item.bandwidth\t%u\n", (unsigned int)
	       btrfs_stack_device_bandwidth(&sb->dev_item));
	printf("dev_item.generation\t%llu\n", (unsigned long long)
	       btrfs_stack_device_generation(&sb->dev_item));
}
