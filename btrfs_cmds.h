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

/* btrfs_cmds.c*/
int do_clone(int nargs, char **argv);
int do_delete_subvolume(int nargs, char **argv);
int do_create_subvol(int nargs, char **argv);
int do_fssync(int nargs, char **argv);
int do_defrag(int argc, char **argv);
int do_show_filesystem(int nargs, char **argv);
int do_add_volume(int nargs, char **args);
int do_balance(int nargs, char **argv);
int do_remove_volume(int nargs, char **args);
int do_scan(int nargs, char **argv);
int do_resize(int nargs, char **argv);
int do_subvol_list(int nargs, char **argv);
int do_set_default_subvol(int nargs, char **argv);
int list_subvols(int fd);
int do_df_filesystem(int nargs, char **argv);
int find_updated_files(int fd, u64 root_id, u64 oldest_gen);
int do_find_newer(int argc, char **argv);
int do_change_label(int argc, char **argv);
