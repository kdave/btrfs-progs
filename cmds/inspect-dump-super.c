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
#include <stdio.h>
#include <unistd.h>
#include <fcntl.h>
#include <sys/stat.h>
#include <errno.h>
#include <getopt.h>

#include "ctree.h"
#include "disk-io.h"
#include "print-tree.h"
#include "common/utils.h"
#include "cmds/commands.h"
#include "common/help.h"

static int load_and_dump_sb(char *filename, int fd, u64 sb_bytenr, int full,
		int force)
{
	u8 super_block_data[BTRFS_SUPER_INFO_SIZE];
	struct btrfs_super_block *sb;
	u64 ret;

	sb = (struct btrfs_super_block *)super_block_data;

	ret = pread64(fd, super_block_data, BTRFS_SUPER_INFO_SIZE, sb_bytenr);
	if (ret != BTRFS_SUPER_INFO_SIZE) {
		/* check if the disk if too short for further superblock */
		if (ret == 0 && errno == 0)
			return 0;

		error("failed to read the superblock on %s at %llu",
				filename, (unsigned long long)sb_bytenr);
		error("error = '%m', errno = %d", errno);
		return 1;
	}
	printf("superblock: bytenr=%llu, device=%s\n", sb_bytenr, filename);
	printf("---------------------------------------------------------\n");
	if (btrfs_super_magic(sb) != BTRFS_MAGIC && !force) {
		error("bad magic on superblock on %s at %llu",
				filename, (unsigned long long)sb_bytenr);
	} else {
		btrfs_print_superblock(sb, full);
	}
	return 0;
}

static const char * const cmd_inspect_dump_super_usage[] = {
	"btrfs inspect-internal dump-super [options] device [device...]",
	"Dump superblock from a device in a textual form",
	"",
	"-f|--full             print full superblock information, backup roots etc.",
	"-a|--all              print information about all superblocks",
	"-s|--super <super>    specify which copy to print out (values: 0, 1, 2)",
	"-F|--force            attempt to dump superblocks with bad magic",
	"--bytenr <offset>     specify alternate superblock offset",
	"",
	"Deprecated syntax:",
	"-s <bytenr>           specify alternate superblock offset, values other than 0, 1, 2",
	"                      will be interpreted as --bytenr for backward compatibility,",
	"                      option renamed for consistency with other tools (eg. check)",
	"-i <super>            specify which copy to print out (values: 0, 1, 2), now moved",
	"                      to -s|--super",
	NULL
};

static int cmd_inspect_dump_super(const struct cmd_struct *cmd,
				  int argc, char **argv)
{
	int all = 0;
	int full = 0;
	int force = 0;
	char *filename;
	int fd = -1;
	int i;
	int ret = 0;
	u64 arg;
	u64 sb_bytenr = btrfs_sb_offset(0);

	while (1) {
		int c;
		enum { GETOPT_VAL_BYTENR = 257 };
		static const struct option long_options[] = {
			{"all", no_argument, NULL, 'a'},
			{"bytenr", required_argument, NULL, GETOPT_VAL_BYTENR },
			{"full", no_argument, NULL, 'f'},
			{"force", no_argument, NULL, 'F'},
			{"super", required_argument, NULL, 's' },
			{NULL, 0, NULL, 0}
		};

		c = getopt_long(argc, argv, "fFai:s:", long_options, NULL);
		if (c < 0)
			break;

		switch (c) {
		case 'i':
			warning(
			    "option -i is deprecated, please use -s or --super");
			arg = arg_strtou64(optarg);
			if (arg >= BTRFS_SUPER_MIRROR_MAX) {
				error("super mirror too big: %llu >= %d",
					arg, BTRFS_SUPER_MIRROR_MAX);
				return 1;
			}
			sb_bytenr = btrfs_sb_offset(arg);
			break;

		case 'a':
			all = 1;
			break;
		case 'f':
			full = 1;
			break;
		case 'F':
			force = 1;
			break;
		case 's':
			arg = arg_strtou64(optarg);
			if (BTRFS_SUPER_MIRROR_MAX <= arg) {
				warning(
		"deprecated use of -s <bytenr> with %llu, assuming --bytenr",
						(unsigned long long)arg);
				sb_bytenr = arg;
			} else {
				sb_bytenr = btrfs_sb_offset(arg);
			}
			all = 0;
			break;
		case GETOPT_VAL_BYTENR:
			arg = arg_strtou64(optarg);
			sb_bytenr = arg;
			all = 0;
			break;
		default:
			usage_unknown_option(cmd, argv);
		}
	}

	if (check_argc_min(argc - optind, 1))
		return 1;

	for (i = optind; i < argc; i++) {
		filename = argv[i];
		fd = open(filename, O_RDONLY);
		if (fd < 0) {
			error("cannot open %s: %m", filename);
			ret = 1;
			goto out;
		}

		if (all) {
			int idx;

			for (idx = 0; idx < BTRFS_SUPER_MIRROR_MAX; idx++) {
				sb_bytenr = btrfs_sb_offset(idx);
				if (load_and_dump_sb(filename, fd,
						sb_bytenr, full, force)) {
					close(fd);
					ret = 1;
					goto out;
				}

				putchar('\n');
			}
		} else {
			load_and_dump_sb(filename, fd, sb_bytenr, full, force);
			putchar('\n');
		}
		close(fd);
	}

out:
	return ret;
}
DEFINE_SIMPLE_COMMAND(inspect_dump_super, "dump-super");
