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
#include <unistd.h>
#include <sys/ioctl.h>
#include <errno.h>
#include <sys/stat.h>
#include <libgen.h>
#include <limits.h>

#include "kerncompat.h"
#include "ioctl.h"

#include "commands.h"

/* btrfs-list.c */
int list_subvols(int fd, int print_parent, int get_default);
int find_updated_files(int fd, u64 root_id, u64 oldest_gen);

static const char subvolume_cmd_group_usage[] =
	"btrfs subvolume <command> <args>";

/*
 * test if path is a directory
 * this function return
 * 0-> path exists but it is not a directory
 * 1-> path exists and it is  a directory
 * -1 -> path is unaccessible
 */
static int test_isdir(char *path)
{
	struct stat	st;
	int		res;

	res = stat(path, &st);
	if(res < 0 )
		return -1;

	return S_ISDIR(st.st_mode);
}

static const char * const cmd_subvol_create_usage[] = {
	"btrfs subvolume create [<dest>/]<name>",
	"Create a subvolume",
	"Create a subvolume <name> in <dest>.  If <dest> is not given",
	"subvolume <name> will be created in the current directory.",
	NULL
};

static int cmd_subvol_create(int argc, char **argv)
{
	int	res, fddst, len, e;
	char	*newname;
	char	*dstdir;
	struct btrfs_ioctl_vol_args	args;
	char	*dst;

	if (check_argc_exact(argc, 2))
		usage(cmd_subvol_create_usage);

	dst = argv[1];

	res = test_isdir(dst);
	if(res >= 0 ){
		fprintf(stderr, "ERROR: '%s' exists\n", dst);
		return 12;
	}

	newname = strdup(dst);
	newname = basename(newname);
	dstdir = strdup(dst);
	dstdir = dirname(dstdir);

	if( !strcmp(newname,".") || !strcmp(newname,"..") ||
	     strchr(newname, '/') ){
		fprintf(stderr, "ERROR: uncorrect subvolume name ('%s')\n",
			newname);
		return 14;
	}

	len = strlen(newname);
	if (len == 0 || len >= BTRFS_VOL_NAME_MAX) {
		fprintf(stderr, "ERROR: subvolume name too long ('%s)\n",
			newname);
		return 14;
	}

	fddst = open_file_or_dir(dstdir);
	if (fddst < 0) {
		fprintf(stderr, "ERROR: can't access to '%s'\n", dstdir);
		return 12;
	}

	printf("Create subvolume '%s/%s'\n", dstdir, newname);
	strncpy(args.name, newname, BTRFS_PATH_NAME_MAX);
	res = ioctl(fddst, BTRFS_IOC_SUBVOL_CREATE, &args);
	e = errno;

	close(fddst);

	if(res < 0 ){
		fprintf( stderr, "ERROR: cannot create subvolume - %s\n",
			strerror(e));
		return 11;
	}

	return 0;
}

/*
 * test if path is a subvolume:
 * this function return
 * 0-> path exists but it is not a subvolume
 * 1-> path exists and it is  a subvolume
 * -1 -> path is unaccessible
 */
static int test_issubvolume(char *path)
{
	struct stat	st;
	int		res;

	res = stat(path, &st);
	if(res < 0 )
		return -1;

	return (st.st_ino == 256) && S_ISDIR(st.st_mode);
}

static const char * const cmd_subvol_delete_usage[] = {
	"btrfs subvolume delete <name>",
	"Delete a subvolume",
	NULL
};

static int cmd_subvol_delete(int argc, char **argv)
{
	int	res, fd, len, e;
	struct btrfs_ioctl_vol_args	args;
	char	*dname, *vname, *cpath;
	char	*path;

	if (check_argc_exact(argc, 2))
		usage(cmd_subvol_delete_usage);

	path = argv[1];

	res = test_issubvolume(path);
	if(res<0){
		fprintf(stderr, "ERROR: error accessing '%s'\n", path);
		return 12;
	}
	if(!res){
		fprintf(stderr, "ERROR: '%s' is not a subvolume\n", path);
		return 13;
	}

	cpath = realpath(path, 0);
	dname = strdup(cpath);
	dname = dirname(dname);
	vname = strdup(cpath);
	vname = basename(vname);
	free(cpath);

	if( !strcmp(vname,".") || !strcmp(vname,"..") ||
	     strchr(vname, '/') ){
		fprintf(stderr, "ERROR: incorrect subvolume name ('%s')\n",
			vname);
		return 14;
	}

	len = strlen(vname);
	if (len == 0 || len >= BTRFS_VOL_NAME_MAX) {
		fprintf(stderr, "ERROR: snapshot name too long ('%s)\n",
			vname);
		return 14;
	}

	fd = open_file_or_dir(dname);
	if (fd < 0) {
		close(fd);
		fprintf(stderr, "ERROR: can't access to '%s'\n", dname);
		return 12;
	}

	printf("Delete subvolume '%s/%s'\n", dname, vname);
	strncpy(args.name, vname, BTRFS_PATH_NAME_MAX);
	res = ioctl(fd, BTRFS_IOC_SNAP_DESTROY, &args);
	e = errno;

	close(fd);

	if(res < 0 ){
		fprintf( stderr, "ERROR: cannot delete '%s/%s' - %s\n",
			dname, vname, strerror(e));
		return 11;
	}

	return 0;
}

static const char * const cmd_subvol_list_usage[] = {
	"btrfs subvolume list [-p] <path>",
	"List subvolumes (and snapshots)",
	"",
	"-p     print parent ID",
	NULL
};

static int cmd_subvol_list(int argc, char **argv)
{
	int fd;
	int ret;
	int print_parent = 0;
	char *subvol;

	optind = 1;
	while(1) {
		int c = getopt(argc, argv, "p");
		if (c < 0)
			break;

		switch(c) {
		case 'p':
			print_parent = 1;
			break;
		default:
			usage(cmd_subvol_list_usage);
		}
	}

	if (check_argc_exact(argc - optind, 1))
		usage(cmd_subvol_list_usage);

	subvol = argv[optind];

	ret = test_issubvolume(subvol);
	if (ret < 0) {
		fprintf(stderr, "ERROR: error accessing '%s'\n", subvol);
		return 12;
	}
	if (!ret) {
		fprintf(stderr, "ERROR: '%s' is not a subvolume\n", subvol);
		return 13;
	}

	fd = open_file_or_dir(subvol);
	if (fd < 0) {
		fprintf(stderr, "ERROR: can't access '%s'\n", subvol);
		return 12;
	}
	ret = list_subvols(fd, print_parent, 0);
	if (ret)
		return 19;
	return 0;
}

static const char * const cmd_snapshot_usage[] = {
	"btrfs subvolume snapshot [-r] <source> [<dest>/]<name>",
	"Create a snapshot of the subvolume",
	"Create a writable/readonly snapshot of the subvolume <source> with",
	"the name <name> in the <dest> directory",
	"",
	"-r     create a readonly snapshot",
	NULL
};

static int cmd_snapshot(int argc, char **argv)
{
	char	*subvol, *dst;
	int	res, fd, fddst, len, e, readonly = 0;
	char	*newname;
	char	*dstdir;
	struct btrfs_ioctl_vol_args_v2	args;

	memset(&args, 0, sizeof(args));

	optind = 1;
	while (1) {
		int c = getopt(argc, argv, "r");
		if (c < 0)
			break;

		switch (c) {
		case 'r':
			readonly = 1;
			break;
		default:
			usage(cmd_snapshot_usage);
		}
	}

	if (check_argc_exact(argc - optind, 2))
		usage(cmd_snapshot_usage);

	subvol = argv[optind];
	dst = argv[optind + 1];

	res = test_issubvolume(subvol);
	if(res<0){
		fprintf(stderr, "ERROR: error accessing '%s'\n", subvol);
		return 12;
	}
	if(!res){
		fprintf(stderr, "ERROR: '%s' is not a subvolume\n", subvol);
		return 13;
	}

	res = test_isdir(dst);
	if(res == 0 ){
		fprintf(stderr, "ERROR: '%s' exists and it is not a directory\n", dst);
		return 12;
	}

	if(res>0){
		newname = strdup(subvol);
		newname = basename(newname);
		dstdir = dst;
	}else{
		newname = strdup(dst);
		newname = basename(newname);
		dstdir = strdup(dst);
		dstdir = dirname(dstdir);
	}

	if( !strcmp(newname,".") || !strcmp(newname,"..") ||
	     strchr(newname, '/') ){
		fprintf(stderr, "ERROR: incorrect snapshot name ('%s')\n",
			newname);
		return 14;
	}

	len = strlen(newname);
	if (len == 0 || len >= BTRFS_VOL_NAME_MAX) {
		fprintf(stderr, "ERROR: snapshot name too long ('%s)\n",
			newname);
		return 14;
	}

	fddst = open_file_or_dir(dstdir);
	if (fddst < 0) {
		fprintf(stderr, "ERROR: can't access to '%s'\n", dstdir);
		return 12;
	}

	fd = open_file_or_dir(subvol);
	if (fd < 0) {
		close(fddst);
		fprintf(stderr, "ERROR: can't access to '%s'\n", dstdir);
		return 12;
	}

	if (readonly) {
		args.flags |= BTRFS_SUBVOL_RDONLY;
		printf("Create a readonly snapshot of '%s' in '%s/%s'\n",
		       subvol, dstdir, newname);
	} else {
		printf("Create a snapshot of '%s' in '%s/%s'\n",
		       subvol, dstdir, newname);
	}

	args.fd = fd;
	strncpy(args.name, newname, BTRFS_SUBVOL_NAME_MAX);
	res = ioctl(fddst, BTRFS_IOC_SNAP_CREATE_V2, &args);
	e = errno;

	close(fd);
	close(fddst);

	if(res < 0 ){
		fprintf( stderr, "ERROR: cannot snapshot '%s' - %s\n",
			subvol, strerror(e));
		return 11;
	}

	return 0;
}

static const char * const cmd_subvol_get_default_usage[] = {
	"btrfs subvolume get-dafault <path>",
	"Get the default subvolume of a filesystem",
	NULL
};

static int cmd_subvol_get_default(int argc, char **argv)
{
	int fd;
	int ret;
	char *subvol;

	if (check_argc_exact(argc, 2))
		usage(cmd_subvol_get_default_usage);

	subvol = argv[1];

	ret = test_issubvolume(subvol);
	if (ret < 0) {
		fprintf(stderr, "ERROR: error accessing '%s'\n", subvol);
		return 12;
	}
	if (!ret) {
		fprintf(stderr, "ERROR: '%s' is not a subvolume\n", subvol);
		return 13;
	}

	fd = open_file_or_dir(subvol);
	if (fd < 0) {
		fprintf(stderr, "ERROR: can't access '%s'\n", subvol);
		return 12;
	}
	ret = list_subvols(fd, 0, 1);
	if (ret)
		return 19;
	return 0;
}

static const char * const cmd_subvol_set_default_usage[] = {
	"btrfs subvolume set-dafault <subvolid> <path>",
	"Set the default subvolume of a filesystem",
	NULL
};

static int cmd_subvol_set_default(int argc, char **argv)
{
	int	ret=0, fd, e;
	u64	objectid;
	char	*path;
	char	*subvolid;

	if (check_argc_exact(argc, 3))
		usage(cmd_subvol_set_default_usage);

	subvolid = argv[1];
	path = argv[2];

	fd = open_file_or_dir(path);
	if (fd < 0) {
		fprintf(stderr, "ERROR: can't access to '%s'\n", path);
		return 12;
	}

	objectid = (unsigned long long)strtoll(subvolid, NULL, 0);
	if (errno == ERANGE) {
		fprintf(stderr, "ERROR: invalid tree id (%s)\n",subvolid);
		return 30;
	}
	ret = ioctl(fd, BTRFS_IOC_DEFAULT_SUBVOL, &objectid);
	e = errno;
	close(fd);
	if( ret < 0 ){
		fprintf(stderr, "ERROR: unable to set a new default subvolume - %s\n",
			strerror(e));
		return 30;
	}
	return 0;
}

static const char * const cmd_find_new_usage[] = {
	"btrfs subvolume find-new <path> <lastgen>",
	"List the recently modified files in a filesystem",
	NULL
};

static int cmd_find_new(int argc, char **argv)
{
	int fd;
	int ret;
	char *subvol;
	u64 last_gen;

	if (check_argc_exact(argc, 3))
		usage(cmd_find_new_usage);

	subvol = argv[1];
	last_gen = atoll(argv[2]);

	ret = test_issubvolume(subvol);
	if (ret < 0) {
		fprintf(stderr, "ERROR: error accessing '%s'\n", subvol);
		return 12;
	}
	if (!ret) {
		fprintf(stderr, "ERROR: '%s' is not a subvolume\n", subvol);
		return 13;
	}

	fd = open_file_or_dir(subvol);
	if (fd < 0) {
		fprintf(stderr, "ERROR: can't access '%s'\n", subvol);
		return 12;
	}
	ret = find_updated_files(fd, 0, last_gen);
	if (ret)
		return 19;
	return 0;
}

const struct cmd_group subvolume_cmd_group = {
	subvolume_cmd_group_usage, NULL, {
		{ "create", cmd_subvol_create, cmd_subvol_create_usage, NULL, 0 },
		{ "delete", cmd_subvol_delete, cmd_subvol_delete_usage, NULL, 0 },
		{ "list", cmd_subvol_list, cmd_subvol_list_usage, NULL, 0 },
		{ "snapshot", cmd_snapshot, cmd_snapshot_usage, NULL, 0 },
		{ "get-default", cmd_subvol_get_default,
			cmd_subvol_get_default_usage, NULL, 0 },
		{ "set-default", cmd_subvol_set_default,
			cmd_subvol_set_default_usage, NULL, 0 },
		{ "find-new", cmd_find_new, cmd_find_new_usage, NULL, 0 },
		{ 0, 0, 0, 0, 0 }
	}
};

int cmd_subvolume(int argc, char **argv)
{
	return handle_command_group(&subvolume_cmd_group, argc, argv);
}
