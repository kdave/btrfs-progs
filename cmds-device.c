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
#include <fcntl.h>
#include <sys/ioctl.h>
#include <errno.h>
#include <sys/stat.h>

#include "kerncompat.h"
#include "ctree.h"
#include "ioctl.h"
#include "utils.h"

#include "btrfs_cmds.h"

/* FIXME - imported cruft, fix sparse errors and warnings */
#ifdef __CHECKER__
#define BLKGETSIZE64 0
#define BTRFS_IOC_SNAP_CREATE_V2 0
#define BTRFS_VOL_NAME_MAX 255
struct btrfs_ioctl_vol_args { char name[BTRFS_VOL_NAME_MAX]; };
static inline int ioctl(int fd, int define, void *arg) { return 0; }
#endif

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

int do_scan(int argc, char **argv)
{
	int	i, fd, e;
	int	checklist = 1;
	int	devstart = 1;

	if( argc >= 2 && !strcmp(argv[1],"--all-devices")){

		if( argc >2 ){
			fprintf(stderr, "ERROR: too may arguments\n");
                        return 22;
                }

		checklist = 0;
		devstart += 1;
	}

	if(argc<=devstart){

		int ret;

		printf("Scanning for Btrfs filesystems\n");
		if(checklist)
			ret = btrfs_scan_block_devices(1);
		else
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

	for( i = devstart ; i < argc ; i++ ){
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

