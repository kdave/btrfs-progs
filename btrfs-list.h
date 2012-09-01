/*
 * Copyright (C) 2012 Fujitsu.  All rights reserved.
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

int list_subvols(int fd, int print_parent, int get_default, int print_uuid);
int list_snapshots(int fd, int print_parent, int order, int print_uuid);
int find_updated_files(int fd, u64 root_id, u64 oldest_gen);
char *path_for_root(int fd, u64 root);
