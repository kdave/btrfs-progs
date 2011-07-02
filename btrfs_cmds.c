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
#include <sys/ioctl.h>
#include <sys/types.h>
#include <dirent.h>
#include <sys/stat.h>
#include <unistd.h>
#include <fcntl.h>
#include <libgen.h>
#include <limits.h>
#include <uuid/uuid.h>
#include <ctype.h>

#undef ULONG_MAX

#include "kerncompat.h"
#include "ctree.h"
#include "transaction.h"
#include "utils.h"
#include "version.h"
#include "ioctl.h"
#include "volumes.h"

#include "btrfs_cmds.h"
#include "btrfslabel.h"

#ifdef __CHECKER__
#define BLKGETSIZE64 0
#define BTRFS_IOC_SNAP_CREATE_V2 0
#define BTRFS_VOL_NAME_MAX 255
struct btrfs_ioctl_vol_args { char name[BTRFS_VOL_NAME_MAX]; };
static inline int ioctl(int fd, int define, void *arg) { return 0; }
#endif

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

static int open_file_or_dir(const char *fname)
{
	int ret;
	struct stat st;
	DIR *dirstream;
	int fd;

	ret = stat(fname, &st);
	if (ret < 0) {
		return -1;
	}
	if (S_ISDIR(st.st_mode)) {
		dirstream = opendir(fname);
		if (!dirstream) {
			return -2;
		}
		fd = dirfd(dirstream);
	} else {
		fd = open(fname, O_RDWR);
	}
	if (fd < 0) {
		return -3;
	}
	return fd;
}

static u64 parse_size(char *s)
{
	int len = strlen(s);
	char c;
	u64 mult = 1;

	if (!isdigit(s[len - 1])) {
		c = tolower(s[len - 1]);
		switch (c) {
		case 'g':
			mult *= 1024;
		case 'm':
			mult *= 1024;
		case 'k':
			mult *= 1024;
		case 'b':
			break;
		default:
			fprintf(stderr, "Unknown size descriptor %c\n", c);
			exit(1);
		}
		s[len - 1] = '\0';
	}
	return atoll(s) * mult;
}

static int parse_compress_type(char *s)
{
	if (strcmp(optarg, "zlib") == 0)
		return BTRFS_COMPRESS_ZLIB;
	else if (strcmp(optarg, "lzo") == 0)
		return BTRFS_COMPRESS_LZO;
	else {
		fprintf(stderr, "Unknown compress type %s\n", s);
		exit(1);
	};
}

int do_defrag(int ac, char **av)
{
	int fd;
	int flush = 0;
	u64 start = 0;
	u64 len = (u64)-1;
	u32 thresh = 0;
	int i;
	int errors = 0;
	int ret = 0;
	int verbose = 0;
	int fancy_ioctl = 0;
	struct btrfs_ioctl_defrag_range_args range;
	int e=0;
	int compress_type = BTRFS_COMPRESS_NONE;

	optind = 1;
	while(1) {
		int c = getopt(ac, av, "vc::fs:l:t:");
		if (c < 0)
			break;
		switch(c) {
		case 'c':
			compress_type = BTRFS_COMPRESS_ZLIB;
			if (optarg)
				compress_type = parse_compress_type(optarg);
			fancy_ioctl = 1;
			break;
		case 'f':
			flush = 1;
			fancy_ioctl = 1;
			break;
		case 'v':
			verbose = 1;
			break;
		case 's':
			start = parse_size(optarg);
			fancy_ioctl = 1;
			break;
		case 'l':
			len = parse_size(optarg);
			fancy_ioctl = 1;
			break;
		case 't':
			thresh = parse_size(optarg);
			fancy_ioctl = 1;
			break;
		default:
			fprintf(stderr, "Invalid arguments for defragment\n");
			free(av);
			return 1;
		}
	}
	if (ac - optind == 0) {
		fprintf(stderr, "Invalid arguments for defragment\n");
		free(av);
		return 1;
	}

	memset(&range, 0, sizeof(range));
	range.start = start;
	range.len = len;
	range.extent_thresh = thresh;
	if (compress_type) {
		range.flags |= BTRFS_DEFRAG_RANGE_COMPRESS;
		range.compress_type = compress_type;
	}
	if (flush)
		range.flags |= BTRFS_DEFRAG_RANGE_START_IO;

	for (i = optind; i < ac; i++) {
		if (verbose)
			printf("%s\n", av[i]);
		fd = open_file_or_dir(av[i]);
		if (fd < 0) {
			fprintf(stderr, "failed to open %s\n", av[i]);
			perror("open:");
			errors++;
			continue;
		}
		if (!fancy_ioctl) {
			ret = ioctl(fd, BTRFS_IOC_DEFRAG, NULL);
			e=errno;
		} else {
			ret = ioctl(fd, BTRFS_IOC_DEFRAG_RANGE, &range);
			if (ret && errno == ENOTTY) {
				fprintf(stderr, "ERROR: defrag range ioctl not "
					"supported in this kernel, please try "
					"without any options.\n");
				errors++;
				close(fd);
				break;
			}
		}
		if (ret) {
			fprintf(stderr, "ERROR: defrag failed on %s - %s\n",
				av[i], strerror(e));
			errors++;
		}
		close(fd);
	}
	if (verbose)
		printf("%s\n", BTRFS_BUILD_VERSION);
	if (errors) {
		fprintf(stderr, "total %d failures\n", errors);
		exit(1);
	}

	free(av);
	return errors + 20;
}

int do_find_newer(int argc, char **argv)
{
	int fd;
	int ret;
	char *subvol;
	u64 last_gen;

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

int do_subvol_list(int argc, char **argv)
{
	int fd;
	int ret;
	int print_parent = 0;
	char *subvol;
        int optind = 1;

	while(1) {
		int c = getopt(argc, argv, "p");
		if (c < 0) break;
		switch(c) {
		case 'p':
			print_parent = 1;
			optind++;
			break;
		}
	}
	
	if (argc - optind != 1) {
		fprintf(stderr, "ERROR: invalid arguments for subvolume list\n");
		return 1;
	}

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
	ret = list_subvols(fd, print_parent);
	if (ret)
		return 19;
	return 0;
}

int do_clone(int argc, char **argv)
{
	char	*subvol, *dst;
	int	res, fd, fddst, len, e, optind = 0, readonly = 0;
	char	*newname;
	char	*dstdir;
	struct btrfs_ioctl_vol_args_v2	args;

	memset(&args, 0, sizeof(args));

	while (1) {
		int c = getopt(argc, argv, "r");

		if (c < 0)
			break;
		switch (c) {
		case 'r':
			optind++;
			readonly = 1;
			break;
		default:
			fprintf(stderr,
				"Invalid arguments for subvolume snapshot\n");
			free(argv);
			return 1;
		}
	}
	if (argc - optind < 2) {
		fprintf(stderr, "Invalid arguments for subvolume snapshot\n");
		free(argv);
		return 1;
	}

	subvol = argv[optind+1];
	dst = argv[optind+2];

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

int do_delete_subvolume(int argc, char **argv)
{
	int	res, fd, len, e;
	struct btrfs_ioctl_vol_args	args;
	char	*dname, *vname, *cpath;
	char	*path = argv[1];

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

int do_create_subvol(int argc, char **argv)
{
	int	res, fddst, len, e;
	char	*newname;
	char	*dstdir;
	struct btrfs_ioctl_vol_args	args;
	char	*dst = argv[1];

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

int do_fssync(int argc, char **argv)
{
	int 	fd, res, e;
	char	*path = argv[1];

	fd = open_file_or_dir(path);
	if (fd < 0) {
		fprintf(stderr, "ERROR: can't access to '%s'\n", path);
		return 12;
	}

	printf("FSSync '%s'\n", path);
	res = ioctl(fd, BTRFS_IOC_SYNC);
	e = errno;
	close(fd);
	if( res < 0 ){
		fprintf(stderr, "ERROR: unable to fs-syncing '%s' - %s\n", 
			path, strerror(e));
		return 16;
	}

	return 0;
}

int do_scan(int argc, char **argv)
{
	int	i, fd, e;
	if(argc<=1){
		int ret;

		printf("Scanning for Btrfs filesystems\n");
		ret = btrfs_scan_one_dir("/dev", 1);
		if (ret){
			fprintf(stderr, "ERROR: error %d while scanning\n", ret);
			return 18;
		}
		return 0;
	}

	fd = open("/dev/btrfs-control", O_RDWR);
	if (fd < 0) {
		perror("failed to open /dev/btrfs-control");
		return 10;
	}

	for( i = 1 ; i < argc ; i++ ){
		struct btrfs_ioctl_vol_args args;
		int ret;

		printf("Scanning for Btrfs filesystems in '%s'\n", argv[i]);

		strncpy(args.name, argv[i], BTRFS_PATH_NAME_MAX);
		/*
		 * FIXME: which are the error code returned by this ioctl ?
		 * it seems that is impossible to understand if there no is
		 * a btrfs filesystem from an I/O error !!!
		 */
		ret = ioctl(fd, BTRFS_IOC_SCAN_DEV, &args);
		e = errno;

		if( ret < 0 ){
			close(fd);
			fprintf(stderr, "ERROR: unable to scan the device '%s' - %s\n", 
				argv[i], strerror(e));
			return 11;
		}
	}

	close(fd);
	return 0;

}

int do_resize(int argc, char **argv)
{

	struct btrfs_ioctl_vol_args	args;
	int	fd, res, len, e;
	char	*amount=argv[1], *path=argv[2];

	fd = open_file_or_dir(path);
	if (fd < 0) {
		fprintf(stderr, "ERROR: can't access to '%s'\n", path);
		return 12;
	}
	len = strlen(amount);
	if (len == 0 || len >= BTRFS_VOL_NAME_MAX) {
		fprintf(stderr, "ERROR: size value too long ('%s)\n",
			amount);
		return 14;
	}

	printf("Resize '%s' of '%s'\n", path, amount);
	strncpy(args.name, amount, BTRFS_PATH_NAME_MAX);
	res = ioctl(fd, BTRFS_IOC_RESIZE, &args);
	e = errno;
	close(fd);
	if( res < 0 ){
		fprintf(stderr, "ERROR: unable to resize '%s' - %s\n", 
			path, strerror(e));
		return 30;
	}
	return 0;
}

static int uuid_search(struct btrfs_fs_devices *fs_devices, char *search)
{
	struct list_head *cur;
	struct btrfs_device *device;

	list_for_each(cur, &fs_devices->devices) {
		device = list_entry(cur, struct btrfs_device, dev_list);
		if ((device->label && strcmp(device->label, search) == 0) ||
		    strcmp(device->name, search) == 0)
			return 1;
	}
	return 0;
}

static void print_one_uuid(struct btrfs_fs_devices *fs_devices)
{
	char uuidbuf[37];
	struct list_head *cur;
	struct btrfs_device *device;
	char *super_bytes_used;
	u64 devs_found = 0;
	u64 total;

	uuid_unparse(fs_devices->fsid, uuidbuf);
	device = list_entry(fs_devices->devices.next, struct btrfs_device,
			    dev_list);
	if (device->label && device->label[0])
		printf("Label: '%s' ", device->label);
	else
		printf("Label: none ");

	super_bytes_used = pretty_sizes(device->super_bytes_used);

	total = device->total_devs;
	printf(" uuid: %s\n\tTotal devices %llu FS bytes used %s\n", uuidbuf,
	       (unsigned long long)total, super_bytes_used);

	free(super_bytes_used);

	list_for_each(cur, &fs_devices->devices) {
		char *total_bytes;
		char *bytes_used;
		device = list_entry(cur, struct btrfs_device, dev_list);
		total_bytes = pretty_sizes(device->total_bytes);
		bytes_used = pretty_sizes(device->bytes_used);
		printf("\tdevid %4llu size %s used %s path %s\n",
		       (unsigned long long)device->devid,
		       total_bytes, bytes_used, device->name);
		free(total_bytes);
		free(bytes_used);
		devs_found++;
	}
	if (devs_found < total) {
		printf("\t*** Some devices missing\n");
	}
	printf("\n");
}

int do_show_filesystem(int argc, char **argv)
{
	struct list_head *all_uuids;
	struct btrfs_fs_devices *fs_devices;
	struct list_head *cur_uuid;
	char *search = argv[1];
	int ret;

	ret = btrfs_scan_one_dir("/dev", 0);
	if (ret){
		fprintf(stderr, "ERROR: error %d while scanning\n", ret);
		return 18;
	}

	all_uuids = btrfs_scanned_uuids();
	list_for_each(cur_uuid, all_uuids) {
		fs_devices = list_entry(cur_uuid, struct btrfs_fs_devices,
					list);
		if (search && uuid_search(fs_devices, search) == 0)
			continue;
		print_one_uuid(fs_devices);
	}
	printf("%s\n", BTRFS_BUILD_VERSION);
	return 0;
}

int do_add_volume(int nargs, char **args)
{

	char	*mntpnt = args[nargs-1];
	int	i, fdmnt, ret=0, e;


	fdmnt = open_file_or_dir(mntpnt);
	if (fdmnt < 0) {
		fprintf(stderr, "ERROR: can't access to '%s'\n", mntpnt);
		return 12;
	}

	for (i = 1; i < (nargs-1); i++ ){
		struct btrfs_ioctl_vol_args ioctl_args;
		int	devfd, res;
		u64 dev_block_count = 0;
		struct stat st;
		int mixed = 0;

		res = check_mounted(args[i]);
		if (res < 0) {
			fprintf(stderr, "error checking %s mount status\n",
				args[i]);
			ret++;
			continue;
		}
		if (res == 1) {
			fprintf(stderr, "%s is mounted\n", args[i]);
			ret++;
			continue;
		}

		devfd = open(args[i], O_RDWR);
		if (!devfd) {
			fprintf(stderr, "ERROR: Unable to open device '%s'\n", args[i]);
			close(devfd);
			ret++;
			continue;
		}
		res = fstat(devfd, &st);
		if (res) {
			fprintf(stderr, "ERROR: Unable to stat '%s'\n", args[i]);
			close(devfd);
			ret++;
			continue;
		}
		if (!S_ISBLK(st.st_mode)) {
			fprintf(stderr, "ERROR: '%s' is not a block device\n", args[i]);
			close(devfd);
			ret++;
			continue;
		}

		res = btrfs_prepare_device(devfd, args[i], 1, &dev_block_count, &mixed);
		if (res) {
			fprintf(stderr, "ERROR: Unable to init '%s'\n", args[i]);
			close(devfd);
			ret++;
			continue;
		}
		close(devfd);

		strncpy(ioctl_args.name, args[i], BTRFS_PATH_NAME_MAX);
		res = ioctl(fdmnt, BTRFS_IOC_ADD_DEV, &ioctl_args);
		e = errno;
		if(res<0){
			fprintf(stderr, "ERROR: error adding the device '%s' - %s\n", 
				args[i], strerror(e));
			ret++;
		}

	}

	close(fdmnt);
	if (ret)
		return ret+20;
	else
		return 0;

}

int do_balance(int argc, char **argv)
{

	int	fdmnt, ret=0, e;
	struct btrfs_ioctl_vol_args args;
	char	*path = argv[1];

	fdmnt = open_file_or_dir(path);
	if (fdmnt < 0) {
		fprintf(stderr, "ERROR: can't access to '%s'\n", path);
		return 12;
	}

	memset(&args, 0, sizeof(args));
	ret = ioctl(fdmnt, BTRFS_IOC_BALANCE, &args);
	e = errno;
	close(fdmnt);
	if(ret<0){
		fprintf(stderr, "ERROR: error during balancing '%s' - %s\n", 
			path, strerror(e));

		return 19;
	}
	return 0;
}
int do_remove_volume(int nargs, char **args)
{

	char	*mntpnt = args[nargs-1];
	int	i, fdmnt, ret=0, e;

	fdmnt = open_file_or_dir(mntpnt);
	if (fdmnt < 0) {
		fprintf(stderr, "ERROR: can't access to '%s'\n", mntpnt);
		return 12;
	}

	for(i=1 ; i < (nargs-1) ; i++ ){
		struct	btrfs_ioctl_vol_args arg;
		int	res;

		strncpy(arg.name, args[i], BTRFS_PATH_NAME_MAX);
		res = ioctl(fdmnt, BTRFS_IOC_RM_DEV, &arg);
		e = errno;
		if(res<0){
			fprintf(stderr, "ERROR: error removing the device '%s' - %s\n", 
				args[i], strerror(e));
			ret++;
		}
	}

	close(fdmnt);
	if( ret)
		return ret+20;
	else
		return 0;
}

int do_set_default_subvol(int nargs, char **argv)
{
	int	ret=0, fd, e;
	u64	objectid;
	char	*path = argv[2];
	char	*subvolid = argv[1];

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

int do_change_label(int nargs, char **argv)
{
	/* check the number of argument */
	if ( nargs > 3 ){
		fprintf(stderr, "ERROR: '%s' requires maximum 2 args\n",
			argv[0]);
		return -2;
	}else if (nargs == 2){
		return get_label(argv[1]);
	} else {	/* nargs == 0 */
		return set_label(argv[1], argv[2]);
	}
}


int do_df_filesystem(int nargs, char **argv)
{
	struct btrfs_ioctl_space_args *sargs;
	u64 count = 0, i;
	int ret;
	int fd;
	int e;
	char *path = argv[1];

	fd = open_file_or_dir(path);
	if (fd < 0) {
		fprintf(stderr, "ERROR: can't access to '%s'\n", path);
		return 12;
	}

	sargs = malloc(sizeof(struct btrfs_ioctl_space_args));
	if (!sargs)
		return -ENOMEM;

	sargs->space_slots = 0;
	sargs->total_spaces = 0;

	ret = ioctl(fd, BTRFS_IOC_SPACE_INFO, sargs);
	e = errno;
	if (ret) {
		fprintf(stderr, "ERROR: couldn't get space info on '%s' - %s\n",
			path, strerror(e));
		free(sargs);
		return ret;
	}
	if (!sargs->total_spaces)
		return 0;

	count = sargs->total_spaces;

	sargs = realloc(sargs, sizeof(struct btrfs_ioctl_space_args) +
			(count * sizeof(struct btrfs_ioctl_space_info)));
	if (!sargs)
		return -ENOMEM;

	sargs->space_slots = count;
	sargs->total_spaces = 0;

	ret = ioctl(fd, BTRFS_IOC_SPACE_INFO, sargs);
	e = errno;
	if (ret) {
		fprintf(stderr, "ERROR: couldn't get space info on '%s' - %s\n",
			path, strerror(e));
		close(fd);
		free(sargs);
		return ret;
	}

	for (i = 0; i < sargs->total_spaces; i++) {
		char description[80];
		char *total_bytes;
		char *used_bytes;
		int written = 0;
		u64 flags = sargs->spaces[i].flags;

		memset(description, 0, 80);

		if (flags & BTRFS_BLOCK_GROUP_DATA) {
			if (flags & BTRFS_BLOCK_GROUP_METADATA) {
				snprintf(description, 15, "%s",
					 "Data+Metadata");
				written += 14;
			} else {
				snprintf(description, 5, "%s", "Data");
				written += 4;
			}
		} else if (flags & BTRFS_BLOCK_GROUP_SYSTEM) {
			snprintf(description, 7, "%s", "System");
			written += 6;
		} else if (flags & BTRFS_BLOCK_GROUP_METADATA) {
			snprintf(description, 9, "%s", "Metadata");
			written += 8;
		}

		if (flags & BTRFS_BLOCK_GROUP_RAID0) {
			snprintf(description+written, 8, "%s", ", RAID0");
			written += 7;
		} else if (flags & BTRFS_BLOCK_GROUP_RAID1) {
			snprintf(description+written, 8, "%s", ", RAID1");
			written += 7;
		} else if (flags & BTRFS_BLOCK_GROUP_DUP) {
			snprintf(description+written, 6, "%s", ", DUP");
			written += 5;
		} else if (flags & BTRFS_BLOCK_GROUP_RAID10) {
			snprintf(description+written, 9, "%s", ", RAID10");
			written += 8;
		}

		total_bytes = pretty_sizes(sargs->spaces[i].total_bytes);
		used_bytes = pretty_sizes(sargs->spaces[i].used_bytes);
		printf("%s: total=%s, used=%s\n", description, total_bytes,
		       used_bytes);
	}
	free(sargs);

	return 0;
}
