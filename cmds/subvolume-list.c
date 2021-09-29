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
#include <getopt.h>
#include "kernel-shared/ctree.h"
#include "btrfs-list.h"
#include "common/help.h"
#include "common/messages.h"
#include "common/open-utils.h"
#include "common/utils.h"
#include "cmds/commands.h"

/*
 * Naming of options:
 * - uppercase for filters and sort options
 * - lowercase for enabling specific items in the output
 */
static const char * const cmd_subvol_list_usage[] = {
	"btrfs subvolume list [options] <path>",
	"List subvolumes and snapshots in the filesystem.",
	"",
	"Path filtering:",
	"-o           print only subvolumes below specified path",
	"-a           print all the subvolumes in the filesystem and",
	"             distinguish absolute and relative path with respect",
	"             to the given <path>",
	"",
	"Field selection:",
	"-p           print parent ID",
	"-c           print the ogeneration of the subvolume",
	"-g           print the generation of the subvolume",
	"-u           print the uuid of subvolumes (and snapshots)",
	"-q           print the parent uuid of the snapshots",
	"-R           print the uuid of the received snapshots",
	"",
	"Type filtering:",
	"-s           list only snapshots",
	"-r           list readonly subvolumes (including snapshots)",
	"-d           list deleted subvolumes that are not yet cleaned",
	"",
	"Other:",
	"-t           print the result as a table",
	"",
	"Sorting:",
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

static struct btrfs_list_comparer_set *btrfs_list_alloc_comparer_set(void)
{
	struct btrfs_list_comparer_set *set;
	int size;

	size = sizeof(struct btrfs_list_comparer_set) +
	       BTRFS_LIST_NCOMPS_INCREASE * sizeof(struct btrfs_list_comparer);
	set = calloc(1, size);
	if (!set) {
		fprintf(stderr, "memory allocation failed\n");
		exit(1);
	}

	set->total = BTRFS_LIST_NCOMPS_INCREASE;

	return set;
}

static int cmd_subvol_list(const struct cmd_struct *cmd, int argc, char **argv)
{
	struct btrfs_list_filter_set *filter_set;
	struct btrfs_list_comparer_set *comparer_set;
	u64 flags = 0;
	int fd = -1;
	u64 top_id;
	int ret = -1, uerr = 0;
	char *subvol;
	int is_list_all = 0;
	int is_only_in_path = 0;
	DIR *dirstream = NULL;
	enum btrfs_list_layout layout = BTRFS_LIST_LAYOUT_DEFAULT;

	filter_set = btrfs_list_alloc_filter_set();
	comparer_set = btrfs_list_alloc_comparer_set();

	optind = 0;
	while(1) {
		int c;
		static const struct option long_options[] = {
			{"sort", required_argument, NULL, 'S'},
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
			layout = BTRFS_LIST_LAYOUT_TABLE;
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

	if (check_argc_exact(argc - optind, 1))
		goto out;

	subvol = argv[optind];
	fd = btrfs_open_dir(subvol, &dirstream, 1);
	if (fd < 0) {
		ret = -1;
		error("can't access '%s'", subvol);
		goto out;
	}

	if (flags)
		btrfs_list_setup_filter(&filter_set, BTRFS_LIST_FILTER_FLAGS,
					flags);

	ret = lookup_path_rootid(fd, &top_id);
	if (ret) {
		errno = -ret;
		error("cannot resolve rootid for path: %m");
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

	ret = btrfs_list_subvols_print(fd, filter_set, comparer_set,
			layout, !is_list_all && !is_only_in_path, NULL);

out:
	close_file_or_dir(fd, dirstream);
	if (filter_set)
		free(filter_set);
	if (comparer_set)
		free(comparer_set);
	if (uerr)
		usage(cmd);
	return !!ret;
}
DEFINE_SIMPLE_COMMAND(subvol_list, "list");
