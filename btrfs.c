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
#include <getopt.h>

#include "volumes.h"
#include "crc32c.h"
#include "commands.h"
#include "utils.h"
#include "help.h"

static const char * const btrfs_cmd_group_usage[] = {
	"btrfs [--help] [--version] <group> [<group>...] <command> [<args>]",
	NULL
};

static const char btrfs_cmd_group_info[] =
	"Use --help as an argument for information on a specific group or command.";

static inline const char *skip_prefix(const char *str, const char *prefix)
{
	size_t len = strlen(prefix);
	return strncmp(str, prefix, len) ? NULL : str + len;
}

static int parse_one_token(const char *arg, const struct cmd_group *grp,
			   const struct cmd_struct **cmd_ret)
{
	const struct cmd_struct *cmd = grp->commands;
	const struct cmd_struct *abbrev_cmd = NULL, *ambiguous_cmd = NULL;

	for (; cmd->token; cmd++) {
		const char *rest;

		rest = skip_prefix(arg, cmd->token);
		if (!rest) {
			if (!prefixcmp(cmd->token, arg)) {
				if (abbrev_cmd) {
					/*
					 * If this is abbreviated, it is
					 * ambiguous. So when there is no
					 * exact match later, we need to
					 * error out.
					 */
					ambiguous_cmd = abbrev_cmd;
				}
				abbrev_cmd = cmd;
			}
			continue;
		}
		if (*rest)
			continue;

		*cmd_ret = cmd;
		return 0;
	}

	if (ambiguous_cmd)
		return -2;

	if (abbrev_cmd) {
		*cmd_ret = abbrev_cmd;
		return 0;
	}

	return -1;
}

static const struct cmd_struct *
parse_command_token(const char *arg, const struct cmd_group *grp)
{
	const struct cmd_struct *cmd = NULL;

	switch(parse_one_token(arg, grp, &cmd)) {
	case -1:
		help_unknown_token(arg, grp);
	case -2:
		help_ambiguous_token(arg, grp);
	}

	return cmd;
}

static void handle_help_options_next_level(const struct cmd_struct *cmd,
		int argc, char **argv)
{
	if (argc < 2)
		return;

	if (!strcmp(argv[1], "--help")) {
		if (cmd->next) {
			argc--;
			argv++;
			help_command_group(cmd->next, argc, argv);
		} else {
			usage_command(cmd, 1, 0);
		}

		exit(0);
	}
}

int handle_command_group(const struct cmd_group *grp, int argc,
			 char **argv)

{
	const struct cmd_struct *cmd;

	argc--;
	argv++;
	if (argc < 1) {
		usage_command_group(grp, 0, 0);
		exit(1);
	}

	cmd = parse_command_token(argv[0], grp);

	handle_help_options_next_level(cmd, argc, argv);

	fixup_argv0(argv, cmd->token);
	return cmd->fn(argc, argv);
}

static const struct cmd_group btrfs_cmd_group;

static const char * const cmd_help_usage[] = {
	"btrfs help [--full]",
	"Display help information",
	"",
	"--full     display detailed help on every command",
	NULL
};

static int cmd_help(int argc, char **argv)
{
	help_command_group(&btrfs_cmd_group, argc, argv);
	return 0;
}

static const char * const cmd_version_usage[] = {
	"btrfs version",
	"Display btrfs-progs version",
	NULL
};

static int cmd_version(int argc, char **argv)
{
	printf("%s\n", PACKAGE_STRING);
	return 0;
}

/*
 * Parse global options, between binary name and first non-option argument
 * after processing all valid options (including those with arguments).
 *
 * Returns index to argv where parsing stopped, optind is reset to 1
 */
static int handle_global_options(int argc, char **argv)
{
	enum { OPT_HELP = 256, OPT_VERSION, OPT_FULL };
	static const struct option long_options[] = {
		{ "help", no_argument, NULL, OPT_HELP },
		{ "version", no_argument, NULL, OPT_VERSION },
		{ "full", no_argument, NULL, OPT_FULL },
		{ NULL, 0, NULL, 0}
	};
	int shift;

	if (argc == 0)
		return 0;

	opterr = 0;
	while (1) {
		int c;

		c = getopt_long(argc, argv, "+", long_options, NULL);
		if (c < 0)
			break;

		switch (c) {
		case OPT_HELP: break;
		case OPT_VERSION: break;
		case OPT_FULL: break;
		default:
			fprintf(stderr, "Unknown global option: %s\n",
					argv[optind - 1]);
			exit(129);
		}
	}

	shift = optind;
	optind = 1;

	return shift;
}

void handle_special_globals(int shift, int argc, char **argv)
{
	int has_help = 0;
	int has_full = 0;
	int i;

	for (i = 0; i < shift; i++) {
		if (strcmp(argv[i], "--help") == 0)
			has_help = 1;
		else if (strcmp(argv[i], "--full") == 0)
			has_full = 1;
	}

	if (has_help) {
		if (has_full)
			usage_command_group(&btrfs_cmd_group, 1, 0);
		else
			cmd_help(argc, argv);
		exit(0);
	}

	for (i = 0; i < shift; i++)
		if (strcmp(argv[i], "--version") == 0) {
			cmd_version(argc, argv);
			exit(0);
		}
}

static const struct cmd_group btrfs_cmd_group = {
	btrfs_cmd_group_usage, btrfs_cmd_group_info, {
		{ "subvolume", cmd_subvolume, NULL, &subvolume_cmd_group, 0 },
		{ "filesystem", cmd_filesystem, NULL, &filesystem_cmd_group, 0 },
		{ "balance", cmd_balance, NULL, &balance_cmd_group, 0 },
		{ "device", cmd_device, NULL, &device_cmd_group, 0 },
		{ "scrub", cmd_scrub, NULL, &scrub_cmd_group, 0 },
		{ "check", cmd_check, cmd_check_usage, NULL, 0 },
		{ "rescue", cmd_rescue, NULL, &rescue_cmd_group, 0 },
		{ "restore", cmd_restore, cmd_restore_usage, NULL, 0 },
		{ "inspect-internal", cmd_inspect, NULL, &inspect_cmd_group, 0 },
		{ "property", cmd_property, NULL, &property_cmd_group, 0 },
		{ "send", cmd_send, cmd_send_usage, NULL, 0 },
		{ "receive", cmd_receive, cmd_receive_usage, NULL, 0 },
		{ "quota", cmd_quota, NULL, &quota_cmd_group, 0 },
		{ "qgroup", cmd_qgroup, NULL, &qgroup_cmd_group, 0 },
		{ "replace", cmd_replace, NULL, &replace_cmd_group, 0 },
		{ "help", cmd_help, cmd_help_usage, NULL, 0 },
		{ "version", cmd_version, cmd_version_usage, NULL, 0 },
		NULL_CMD_STRUCT
	},
};

int main(int argc, char **argv)
{
	const struct cmd_struct *cmd;
	const char *bname;
	int ret;

	btrfs_config_init();

	if ((bname = strrchr(argv[0], '/')) != NULL)
		bname++;
	else
		bname = argv[0];

	if (!strcmp(bname, "btrfsck")) {
		argv[0] = "check";
	} else {
		int shift;

		shift = handle_global_options(argc, argv);
		handle_special_globals(shift, argc, argv);
		while (shift-- > 0) {
			argc--;
			argv++;
		}
		if (argc == 0) {
			usage_command_group_short(&btrfs_cmd_group);
			exit(1);
		}
	}

	cmd = parse_command_token(argv[0], &btrfs_cmd_group);

	handle_help_options_next_level(cmd, argc, argv);

	crc32c_optimization_init();

	fixup_argv0(argv, cmd->token);

	ret = cmd->fn(argc, argv);

	btrfs_close_all_devices();

	exit(ret);
}
