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

#ifndef __CMDS_FI_DISK_USAGE__
#define __CMDS_FI_DISK_USAGE__

extern const char * const cmd_filesystem_df_usage[];
int cmd_filesystem_df(int argc, char **argv);

extern const char * const cmd_filesystem_disk_usage_usage[];
int cmd_filesystem_disk_usage(int argc, char **argv);

#endif
