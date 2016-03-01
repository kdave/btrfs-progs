/*
 * Copyright (C) 2007 Oracle.  All rights reserved.
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

#include "disk-io.h"
#include "volumes.h"
#include "utils.h"
#include "commands.h"
#include "cmds-inspect-dump-tree.h"

int main(int argc, char **argv)
{
	int ret;

	set_argv0(argv);

	if (argc > 1 && !strcmp(argv[1], "--help"))
		usage(cmd_inspect_dump_tree_usage);

	radix_tree_init();

	ret = cmd_inspect_dump_tree(argc, argv);

	btrfs_close_all_devices();

	return ret;
}
