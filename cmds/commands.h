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

#ifndef __BTRFS_COMMANDS_H__
#define __BTRFS_COMMANDS_H__

enum {
	CMD_HIDDEN = (1 << 0),	/* should not be in help listings */
	CMD_ALIAS = (1 << 1),	/* alias of next command in cmd_group */
	CMD_FORMAT_TEXT = (1 << 2),	/* output as plain text */
	CMD_FORMAT_JSON = (1 << 3),	/* output in json */
};

#define CMD_FORMAT_MASK		(CMD_FORMAT_TEXT | CMD_FORMAT_JSON)

struct cmd_struct {
	const char *token;
	int (*fn)(const struct cmd_struct *cmd, int argc, char **argv);

	/*
	 * Usage strings
	 *
	 * A NULL-terminated array of the following format:
	 *
	 *   usagestr[0] - one-line synopsis (required)
	 *   usagestr[1] - one-line short description (required)
	 *   usagestr[2..m] - a long (possibly multi-line) description
	 *                    (optional)
	 *   usagestr[m + 1] - an empty line separator (required if at least one
	 *                     option string is given, not needed otherwise)
	 *   usagestr[m + 2..n] - option strings, one option per line
	 *                        (optional)
	 *   usagestr[n + 1] - NULL terminator
	 *
	 * Options (if present) should always (even if there is no long
	 * description) be prepended with an empty line.  Supplied strings are
	 * indented but otherwise printed as-is, no automatic wrapping is done.
	 *
	 * Grep for cmd_*_usage[] for examples.
	 */
	const char * const *usagestr;

	/* should be NULL if token is not a subgroup */
	const struct cmd_group *next;

	/* CMD_* flags above */
	int flags;
};

/*
 * These macros will create cmd_struct structures with a standard name:
 * cmd_struct_<name>.
 */
#define __CMD_NAME(name)	cmd_struct_ ##name
#define DECLARE_COMMAND(name)						\
	extern const struct cmd_struct __CMD_NAME(name)

/* Define a command with all members specified */
#define DEFINE_COMMAND(name, _token, _fn, _usagestr, _group, _flags)	\
	const struct cmd_struct __CMD_NAME(name) =			\
		{							\
			.token = (_token),				\
			.fn = (_fn),					\
			.usagestr = (_usagestr),			\
			.next = (_group),				\
			.flags = CMD_FORMAT_TEXT | (_flags),		\
		}

/*
 * Define a command for the common case - just a name and string.
 * It's assumed that the callback is called cmd_<name> and the usage
 * array is named cmd_<name>_usage.
 */
#define DEFINE_SIMPLE_COMMAND(name, token)				\
	DEFINE_COMMAND(name, token, cmd_ ##name,			\
		       cmd_ ##name ##_usage, NULL, 0)

/*
 * Define a command with flags, eg. with the additional output formats.
 * See CMD_* .
 */
#define DEFINE_COMMAND_WITH_FLAGS(name, token, flags)			\
	DEFINE_COMMAND(name, token, cmd_ ##name,			\
		       cmd_ ##name ##_usage, NULL, (flags))

/*
 * Define a command group callback.
 * It's assumed that the callback is called cmd_<name> and the
 * struct cmd_group is called <name>_cmd_group.
 */
#define DEFINE_GROUP_COMMAND(name, token)				\
	DEFINE_COMMAND(name, token, handle_command_group,		\
		       NULL, &(name ## _cmd_group), 0)

/*
 * Define a command group callback when the name and the string are
 * the same.
 */
#define DEFINE_GROUP_COMMAND_TOKEN(name)				\
	DEFINE_GROUP_COMMAND(name, #name)

struct cmd_group {
	const char * const *usagestr;
	const char *infostr;

	const struct cmd_struct * const commands[];
};

static inline int cmd_execute(const struct cmd_struct *cmd,
			      int argc, char **argv)
{
	return cmd->fn(cmd, argc, argv);
}

int handle_command_group(const struct cmd_struct *cmd, int argc, char **argv);

extern const char * const generic_cmd_help_usage[];

DECLARE_COMMAND(subvolume);
DECLARE_COMMAND(filesystem);
DECLARE_COMMAND(filesystem_du);
DECLARE_COMMAND(filesystem_usage);
DECLARE_COMMAND(balance);
DECLARE_COMMAND(device);
DECLARE_COMMAND(scrub);
DECLARE_COMMAND(check);
DECLARE_COMMAND(chunk_recover);
DECLARE_COMMAND(super_recover);
DECLARE_COMMAND(inspect);
DECLARE_COMMAND(inspect_dump_super);
DECLARE_COMMAND(inspect_dump_tree);
DECLARE_COMMAND(inspect_tree_stats);
DECLARE_COMMAND(property);
DECLARE_COMMAND(send);
DECLARE_COMMAND(receive);
DECLARE_COMMAND(quota);
DECLARE_COMMAND(qgroup);
DECLARE_COMMAND(replace);
DECLARE_COMMAND(restore);
DECLARE_COMMAND(select_super);
DECLARE_COMMAND(dump_super);
DECLARE_COMMAND(debug_tree);
DECLARE_COMMAND(rescue);

#endif
