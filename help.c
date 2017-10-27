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
#include <limits.h>
#include <getopt.h>

#include "commands.h"
#include "utils.h"
#include "help.h"

#define USAGE_SHORT		1U
#define USAGE_LONG		2U
#define USAGE_OPTIONS		4U
#define USAGE_LISTING		8U

static char argv0_buf[ARGV0_BUF_SIZE] = "btrfs";

const char *get_argv0_buf(void)
{
	return argv0_buf;
}

void fixup_argv0(char **argv, const char *token)
{
	int len = strlen(argv0_buf);

	snprintf(argv0_buf + len, sizeof(argv0_buf) - len, " %s", token);
	argv[0] = argv0_buf;
}

void set_argv0(char **argv)
{
	strncpy(argv0_buf, argv[0], sizeof(argv0_buf));
	argv0_buf[sizeof(argv0_buf) - 1] = 0;
}

int check_argc_exact(int nargs, int expected)
{
	if (nargs < expected)
		fprintf(stderr, "%s: too few arguments\n", argv0_buf);
	if (nargs > expected)
		fprintf(stderr, "%s: too many arguments\n", argv0_buf);

	return nargs != expected;
}

int check_argc_min(int nargs, int expected)
{
	if (nargs < expected) {
		fprintf(stderr, "%s: too few arguments\n", argv0_buf);
		return 1;
	}

	return 0;
}

int check_argc_max(int nargs, int expected)
{
	if (nargs > expected) {
		fprintf(stderr, "%s: too many arguments\n", argv0_buf);
		return 1;
	}

	return 0;
}

/*
 * Preprocess @argv with getopt_long to reorder options and consume the "--"
 * option separator.
 * Unknown short and long options are reported, optionally the @usage is printed
 * before exit.
 */
void clean_args_no_options(int argc, char *argv[], const char * const *usagestr)
{
	static const struct option long_options[] = {
		{NULL, 0, NULL, 0}
	};

	while (1) {
		int c = getopt_long(argc, argv, "", long_options, NULL);

		if (c < 0)
			break;

		switch (c) {
		default:
			if (usagestr)
				usage(usagestr);
		}
	}
}

/*
 * Same as clean_args_no_options but pass through arguments that could look
 * like short options. Eg. reisze which takes a negative resize argument like
 * '-123M' .
 *
 * This accepts only two forms:
 * - "-- option1 option2 ..."
 * - "option1 option2 ..."
 */
void clean_args_no_options_relaxed(int argc, char *argv[], const char * const *usagestr)
{
	if (argc <= 1)
		return;

	if (strcmp(argv[1], "--") == 0)
		optind = 2;
}

static int do_usage_one_command(const char * const *usagestr,
				unsigned int flags, FILE *outf)
{
	int pad = 4;
	const char *prefix = "usage: ";
	const char *pad_listing = "    ";

	if (!usagestr || !*usagestr)
		return -1;

	if (flags & USAGE_LISTING)
		prefix = pad_listing;

	fputs(prefix, outf);
	if (strchr(*usagestr, '\n') == NULL) {
		fputs(*usagestr, outf);
	} else {
		const char *c = *usagestr;
		const char *nprefix = "       ";

		if (flags & USAGE_LISTING)
			nprefix = pad_listing;

		for (c = *usagestr; *c; c++) {
			fputc(*c, outf);
			if (*c == '\n')
				fputs(nprefix, outf);
		}
	}
	usagestr++;

	/* a short one-line description (mandatory) */
	if ((flags & USAGE_SHORT) == 0)
		return 0;
	else if (!*usagestr)
		return -2;
	fputc('\n', outf);

	if (flags & USAGE_LISTING)
		pad = 8;
	else
		fputc('\n', outf);

	fprintf(outf, "%*s%s\n", pad, "", *usagestr++);

	/* a long (possibly multi-line) description (optional) */
	if (!*usagestr || ((flags & USAGE_LONG) == 0))
		return 0;

	if (**usagestr)
		fputc('\n', outf);
	while (*usagestr && **usagestr)
		fprintf(outf, "%*s%s\n", pad, "", *usagestr++);

	/* options (optional) */
	if (!*usagestr || ((flags & USAGE_OPTIONS) == 0))
		return 0;

	/*
	 * options (if present) should always (even if there is no long
	 * description) be prepended with an empty line, skip it
	 */
	usagestr++;

	fputc('\n', outf);
	while (*usagestr)
		fprintf(outf, "%*s%s\n", pad, "", *usagestr++);

	return 0;
}

static int usage_command_internal(const char * const *usagestr,
				  const char *token, int full, int lst,
				  int alias, FILE *outf)
{
	unsigned int flags = 0;
	int ret;

	if (!alias)
		flags |= USAGE_SHORT;
	if (full)
		flags |= USAGE_LONG | USAGE_OPTIONS;
	if (lst)
		flags |= USAGE_LISTING;

	ret = do_usage_one_command(usagestr, flags, outf);
	switch (ret) {
	case -1:
		fprintf(outf, "No usage for '%s'\n", token);
		break;
	case -2:
		fprintf(outf, "No short description for '%s'\n", token);
		break;
	}

	return ret;
}

static void usage_command_usagestr(const char * const *usagestr,
				   const char *token, int full, int err)
{
	FILE *outf = err ? stderr : stdout;
	int ret;

	ret = usage_command_internal(usagestr, token, full, 0, 0, outf);
	if (!ret)
		fputc('\n', outf);
}

void usage_command(const struct cmd_struct *cmd, int full, int err)
{
	usage_command_usagestr(cmd->usagestr, cmd->token, full, err);
}

__attribute__((noreturn))
void usage(const char * const *usagestr)
{
	usage_command_usagestr(usagestr, NULL, 1, 1);
	exit(1);
}

static void usage_command_group_internal(const struct cmd_group *grp, int full,
					 FILE *outf)
{
	const struct cmd_struct *cmd = grp->commands;
	int do_sep = 0;

	for (; cmd->token; cmd++) {
		if (cmd->flags & CMD_HIDDEN)
			continue;

		if (full && cmd != grp->commands)
			fputc('\n', outf);

		if (!cmd->next) {
			if (do_sep) {
				fputc('\n', outf);
				do_sep = 0;
			}

			usage_command_internal(cmd->usagestr, cmd->token, full,
					       1, cmd->flags & CMD_ALIAS, outf);
			if (cmd->flags & CMD_ALIAS)
				putchar('\n');
			continue;
		}

		/* this is an entry point to a nested command group */

		if (!full && cmd != grp->commands)
			fputc('\n', outf);

		usage_command_group_internal(cmd->next, full, outf);

		if (!full)
			do_sep = 1;
	}
}

void usage_command_group_short(const struct cmd_group *grp)
{
	const char * const *usagestr = grp->usagestr;
	FILE *outf = stdout;
	const struct cmd_struct *cmd;

	if (usagestr && *usagestr) {
		fprintf(outf, "usage: %s\n", *usagestr++);
		while (*usagestr)
			fprintf(outf, "   or: %s\n", *usagestr++);
	}

	fputc('\n', outf);

	fprintf(outf, "Command groups:\n");
	for (cmd = grp->commands; cmd->token; cmd++) {
		if (cmd->flags & CMD_HIDDEN)
			continue;

		if (!cmd->next)
			continue;

		fprintf(outf, "  %-16s  %s\n", cmd->token, cmd->next->infostr);
	}

	fprintf(outf, "\nCommands:\n");
	for (cmd = grp->commands; cmd->token; cmd++) {
		if (cmd->flags & CMD_HIDDEN)
			continue;

		if (cmd->next)
			continue;

		fprintf(outf, "  %-16s  %s\n", cmd->token, cmd->usagestr[1]);
	}

	fputc('\n', outf);
	fprintf(stderr, "For an overview of a given command use 'btrfs command --help'\n");
	fprintf(stderr, "or 'btrfs [command...] --help --full' to print all available options.\n");
	fprintf(stderr, "Any command name can be shortened as far as it stays unambiguous,\n");
	fprintf(stderr, "however it is recommended to use full command names in scripts.\n");
	fprintf(stderr, "All command groups have their manual page named 'btrfs-<group>'.\n");
}

void usage_command_group(const struct cmd_group *grp, int full, int err)
{
	const char * const *usagestr = grp->usagestr;
	FILE *outf = err ? stderr : stdout;

	if (usagestr && *usagestr) {
		fprintf(outf, "usage: %s\n", *usagestr++);
		while (*usagestr)
			fprintf(outf, "   or: %s\n", *usagestr++);
	}

	fputc('\n', outf);
	usage_command_group_internal(grp, full, outf);
	fputc('\n', outf);

	if (grp->infostr)
		fprintf(outf, "%s\n", grp->infostr);
}

__attribute__((noreturn))
void help_unknown_token(const char *arg, const struct cmd_group *grp)
{
	fprintf(stderr, "%s: unknown token '%s'\n", get_argv0_buf(), arg);
	usage_command_group(grp, 0, 1);
	exit(1);
}

__attribute__((noreturn))
void help_ambiguous_token(const char *arg, const struct cmd_group *grp)
{
	const struct cmd_struct *cmd = grp->commands;

	fprintf(stderr, "%s: ambiguous token '%s'\n", get_argv0_buf(), arg);
	fprintf(stderr, "\nDid you mean one of these ?\n");

	for (; cmd->token; cmd++) {
		if (!prefixcmp(cmd->token, arg))
			fprintf(stderr, "\t%s\n", cmd->token);
	}

	exit(1);
}

void help_command_group(const struct cmd_group *grp, int argc, char **argv)
{
	int full = 0;

	if (argc > 1) {
		if (!strcmp(argv[1], "--full"))
			full = 1;
	}

	usage_command_group(grp, full, 0);
}

