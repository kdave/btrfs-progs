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
int do_scrub_start(int nargs, char **argv);
int do_scrub_status(int argc, char **argv);
int do_scrub_resume(int argc, char **argv);
int do_scrub_cancel(int nargs, char **argv);
int do_remove_volume(int nargs, char **args);
int do_scan(int nargs, char **argv);
int do_resize(int nargs, char **argv);
int do_subvol_list(int nargs, char **argv);
int do_set_default_subvol(int nargs, char **argv);
int do_get_default_subvol(int nargs, char **argv);
int do_df_filesystem(int nargs, char **argv);
int do_find_newer(int argc, char **argv);
int do_change_label(int argc, char **argv);
int open_file_or_dir(const char *fname);
int do_ino_to_path(int nargs, char **argv);
int do_logical_to_ino(int nargs, char **argv);
