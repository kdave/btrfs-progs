/*
 * Copyright (C) 2013 SUSE.  All rights reserved.
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

#include "kerncompat.h"

#include "commands.h"

static const char * const rescue_cmd_group_usage[] = {
	"btrfs rescue <command> [options] <path>",
	NULL
};

const struct cmd_group rescue_cmd_group = {
	rescue_cmd_group_usage, NULL, {
		{ "chunk-recover", cmd_chunk_recover, cmd_chunk_recover_usage, NULL, 0},
		{ 0, 0, 0, 0, 0 }
	}
};

int cmd_rescue(int argc, char **argv)
{
	return handle_command_group(&rescue_cmd_group, argc, argv);
}
