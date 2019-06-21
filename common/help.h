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

#ifndef __BTRFS_HELP_H__
#define __BTRFS_HELP_H__

#define GETOPT_VAL_SI				256
#define GETOPT_VAL_IEC				257
#define GETOPT_VAL_RAW				258
#define GETOPT_VAL_HUMAN_READABLE		259
#define GETOPT_VAL_KBYTES			260
#define GETOPT_VAL_MBYTES			261
#define GETOPT_VAL_GBYTES			262
#define GETOPT_VAL_TBYTES			263

#define GETOPT_VAL_HELP				270

#define ARGV0_BUF_SIZE	PATH_MAX

#define HELPINFO_UNITS_LONG							\
	"--raw              raw numbers in bytes",				\
	"--human-readable   human friendly numbers, base 1024 (default)",	\
	"--iec              use 1024 as a base (KiB, MiB, GiB, TiB)",		\
	"--si               use 1000 as a base (kB, MB, GB, TB)",		\
	"--kbytes           show sizes in KiB, or kB with --si",		\
	"--mbytes           show sizes in MiB, or MB with --si",		\
	"--gbytes           show sizes in GiB, or GB with --si",		\
	"--tbytes           show sizes in TiB, or TB with --si"

#define HELPINFO_UNITS_SHORT_LONG						\
	"-b|--raw           raw numbers in bytes",				\
	"-h|--human-readable",							\
	"                   human friendly numbers, base 1024 (default)",	\
	"-H                 human friendly numbers, base 1000",			\
	"--iec              use 1024 as a base (KiB, MiB, GiB, TiB)",		\
	"--si               use 1000 as a base (kB, MB, GB, TB)",		\
	"-k|--kbytes        show sizes in KiB, or kB with --si",		\
	"-m|--mbytes        show sizes in MiB, or MB with --si",		\
	"-g|--gbytes        show sizes in GiB, or GB with --si",		\
	"-t|--tbytes        show sizes in TiB, or TB with --si"

/*
 * Special marker in the help strings that will preemptively insert the global
 * options and then continue with the following text that possibly follows
 * after the regular options
 */
#define HELPINFO_INSERT_GLOBALS		"INSERT_GLOBALS"

struct cmd_struct;
struct cmd_group;

/*
 * Descriptor of output format
 */
struct format_desc {
	unsigned int value;
	char name[8];
};

extern const struct format_desc output_formats[2];

const char *output_format_name(unsigned int value);

__attribute__((noreturn))
void usage_unknown_option(const struct cmd_struct *cmd, char **argv);

__attribute__((noreturn))
void usage(const struct cmd_struct *cmd);
void usage_command(const struct cmd_struct *cmd, bool full, bool err);
void usage_command_group(const struct cmd_group *grp, bool all, bool err);
void usage_command_group_short(const struct cmd_group *grp);

__attribute__((noreturn))
void help_unknown_token(const char *arg, const struct cmd_group *grp);
__attribute__((noreturn))
void help_ambiguous_token(const char *arg, const struct cmd_group *grp);

void help_command_group(const struct cmd_group *grp, int argc, char **argv);

int check_argc_exact(int nargs, int expected);
int check_argc_min(int nargs, int expected);
int check_argc_max(int nargs, int expected);
void clean_args_no_options(const struct cmd_struct *cmd,
			   int argc, char *argv[]);
void clean_args_no_options_relaxed(const struct cmd_struct *cmd,
				   int argc, char *argv[]);

void fixup_argv0(char **argv, const char *token);
void set_argv0(char **argv);
const char *get_argv0_buf(void);

#endif
