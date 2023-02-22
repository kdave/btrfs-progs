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
#include <sys/stat.h>
#include <fcntl.h>
#include <stdlib.h>
#include <getopt.h>
#include <unistd.h>
#include "kernel-lib/list.h"
#include "common/messages.h"
#include "common/open-utils.h"
#include "common/parse-utils.h"
#include "common/help.h"
#include "cmds/commands.h"

static const char * const reflink_cmd_group_usage[] = {
	"btrfs reflink <command> <args>",
	NULL
};

static const char * const cmd_reflink_clone_usage[] = {
	"btrfs reflink clone [options] source target",
	"Lightweight file copy",
	"Lightweight file copy, extents are cloned and COW if changed. Multiple",
	"ranges can be specified, source and target file can be the same,",
	"ranges can be combined from both and processed in the order.",
	"",
	"Options:",
	OPTLINE("-s RANGESPEC", "take range spec from the source file"),
	OPTLINE("-t RANGESPEC", "take range from the target file"),
	"",
	"RANGESPEC has three parts and is of format SRCOFF:LENGTH:DESTOFF,",
	"where SRCOFF is offset in the respective file, LENGTH is range length,",
	"DESTOFF is offset in the destination file (always target).",
	"All three values accept the size suffix (k/m/g/t/p/e, case insensitive).",
	NULL
};

struct reflink_range {
	struct list_head list;
	u64 from;
	u64 length;
	u64 to;
	bool same_file;
};

void parse_reflink_range(const char *str, u64 *from, u64 *length, u64 *to)
{
	char tmp[512];
	int i;

	/* Parse from */
	i = 0;
	while (*str && i < sizeof(tmp) && *str != ':')
		tmp[i++] = *str++;
	if (i >= sizeof(tmp)) {
		error("range spec too long");
		exit(1);
	}
	if (*str != ':') {
		error("wrong range spec near %s", str);
		exit(1);
	}
	*from = parse_size_from_string(tmp);
	str++;

	/* Parse length */
	i = 0;
	while (*str && i < sizeof(tmp) && *str != ':')
		tmp[i++] = *str++;
	if (i >= sizeof(tmp)) {
		error("range spec too long");
		exit(1);
	}
	if (*str != ':') {
		error("wrong range spec near %s", str);
		exit(1);
	}
	*length = parse_size_from_string(tmp);
	str++;

	/* Parse to, until end of string */
	*to = parse_size_from_string(str);
}

static int reflink_apply_range(int fd_in, int fd_out, const struct reflink_range *range)
{
	return -EOPNOTSUPP;
}

static int cmd_reflink_clone(const struct cmd_struct *cmd, int argc, char **argv)
{
	int ret = 0;
	LIST_HEAD(ranges);
	struct reflink_range *range = NULL, *tmp, whole;
	const char *source, *target;
	int fd_source = -1, fd_target = -1;

	optind = 0;
	while (1) {
		int c = getopt(argc, argv, "r:s:");
		bool same_file = false;

		if (c < 0)
			break;

		switch (c) {
		case 's':
			same_file = true;
			fallthrough;
		case 'r':
			range = malloc(sizeof(struct reflink_range));
			if (!range) {
				error("not enough memory");
				return 1;
			}
			INIT_LIST_HEAD(&range->list);
			range->same_file = same_file;
			parse_reflink_range(optarg, &range->from, &range->length, &range->to);
			list_add_tail(&range->list, &ranges);
			pr_verbose(LOG_DEBUG, "ADD: %llu:%llu:%llu\n", range->from, range->length, range->to);
			break;
		default:
			usage_unknown_option(cmd, argv);
		}
	}

	if (check_argc_exact(argc - optind, 2))
		return 1;

	source = argv[optind];
	target = argv[optind + 1];
	pr_verbose(LOG_DEFAULT, "Source: %s\n", source);
	pr_verbose(LOG_DEFAULT, "Target: %s\n", target);

	fd_source = open(source, O_RDONLY);
	if (fd_source == -1) {
		error("cannot open source file: %m");
		ret = 1;
		goto out;
	}
	if (fd_target == -1) {
		error("cannot open target file: %m");
		ret = 1;
		goto out;
	}

	if (list_empty(&ranges)) {
		struct stat st;

		ret = fstat(fd_source, &st);
		if (ret == -1) {
			error("cannot fstat target file to determine size: %m");
			goto out;
		}

		pr_verbose(LOG_DEFAULT, "No ranges, use entire flile\n");
		whole.from = 0;
		whole.length = st.st_size;
		whole.to = 0;
		ret = reflink_apply_range(fd_source, fd_target, &whole);
	} else {
		list_for_each_entry(range, &ranges, list) {
			pr_verbose(LOG_DEFAULT, "Range: %llu:%llu:%llu\n", range->from, range->length, range->to);
			ret = reflink_apply_range(fd_source, fd_target, range);
		}

	}
out:
	list_for_each_entry_safe(range, tmp, &ranges, list) {
		free(range);
	}
	if (fd_source != -1)
		close(fd_source);
	if (fd_target != -1)
		close(fd_target);
	return !!ret;
}
static DEFINE_SIMPLE_COMMAND(reflink_clone, "clone");

static const char reflink_cmd_group_info[] =
"reflink, shallow file copies: clone";

static const struct cmd_group reflink_cmd_group = {
	reflink_cmd_group_usage, reflink_cmd_group_info, {
		&cmd_struct_reflink_clone,
		NULL
	}
};

DEFINE_GROUP_COMMAND_TOKEN(reflink);
