#include <stdio.h>
#include <stdlib.h>
#include "kerncompat.h"
#include "ioctl.h"

static unsigned long ioctls[] = {
	BTRFS_IOC_SNAP_CREATE,
	BTRFS_IOC_DEFRAG,
	BTRFS_IOC_RESIZE,
	BTRFS_IOC_SCAN_DEV,
	BTRFS_IOC_TRANS_START,
	BTRFS_IOC_TRANS_END,
	BTRFS_IOC_SYNC,
	BTRFS_IOC_CLONE,
	BTRFS_IOC_ADD_DEV,
	BTRFS_IOC_RM_DEV,
	BTRFS_IOC_BALANCE,
	BTRFS_IOC_SUBVOL_CREATE,
	BTRFS_IOC_SNAP_DESTROY,
	BTRFS_IOC_DEFRAG_RANGE,
	BTRFS_IOC_TREE_SEARCH,
	BTRFS_IOC_INO_LOOKUP,
	BTRFS_IOC_DEFAULT_SUBVOL,
	BTRFS_IOC_SPACE_INFO,
	BTRFS_IOC_SNAP_CREATE_V2,
	0 };

int main(int ac, char **av)
{
	int i = 0;
	while(ioctls[i]) {
		printf("%lu\n" ,ioctls[i]);
		i++;
	}
	return 0;
}

