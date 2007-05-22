#ifndef __CHECKER__
#include <sys/ioctl.h>
#include <sys/mount.h>
#include "ioctl.h"
#endif
#include <stdio.h>
#include <stdlib.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <unistd.h>
#include <dirent.h>
#include "kerncompat.h"

#ifdef __CHECKER__
#define BLKGETSIZE64 0
#define BTRFS_IOC_SNAP_CREATE 0
#define BTRFS_IOC_ADD_DISK 0
#define BTRFS_VOL_NAME_MAX 255
struct btrfs_ioctl_vol_args { char name[BTRFS_VOL_NAME_MAX]; };
static inline int ioctl(int fd, int define, void *arg) { return 0; }
#endif

void print_usage(void)
{
	printf("usage: btrfsctl [ -s snapshot_name ] dir\n");
	exit(1);
}

int main(int ac, char **av)
{
	char *fname;
	int fd;
	int ret;
	struct btrfs_ioctl_vol_args args;
	char *name = NULL;
	int i;
	struct stat st;
	DIR *dirstream;
	unsigned long command = 0;

	for (i = 1; i < ac - 1; i++) {
		if (strcmp(av[i], "-s") == 0) {
			if (i + 1 >= ac - 1) {
				fprintf(stderr, "-s requires an arg");
				print_usage();
			}
			name = av[i + 1];
			if (strlen(name) >= BTRFS_VOL_NAME_MAX) {
				fprintf(stderr, "snapshot name is too long\n");
				exit(1);
			}
			command = BTRFS_IOC_SNAP_CREATE;
		}
		if (strcmp(av[i], "-a") == 0) {
			if (i + 1 >= ac - 1) {
				fprintf(stderr, "-a requires an arg");
				print_usage();
			}
			name = av[i + 1];
			if (strlen(name) >= BTRFS_VOL_NAME_MAX) {
				fprintf(stderr, "device name is too long\n");
				exit(1);
			}
			command = BTRFS_IOC_ADD_DISK;
		}
	}
	if (command == 0) {
		fprintf(stderr, "no valid commands given\n");
		exit(1);
	}
	fname = av[ac - 1];
printf("fname is %s\n", fname);
	ret = stat(fname, &st);
	if (ret < 0) {
		perror("stat:");
		exit(1);
	}
	if (S_ISDIR(st.st_mode)) {
		dirstream = opendir(fname);
		if (!dirstream) {
			perror("opendir");
			exit(1);
		}
		fd = dirfd(dirstream);
	} else {
		fd = open(fname, O_RDWR);
	} if (fd < 0) {
		perror("open");
		exit(1);
	}
	strcpy(args.name, name);
	ret = ioctl(fd, command, &args);
	printf("ioctl returns %d\n", ret);
	return 0;
}

