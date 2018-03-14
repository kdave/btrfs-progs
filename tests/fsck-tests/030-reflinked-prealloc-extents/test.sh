#!/bin/bash
#
# Verify that a filesystem check operation (fsck) does not report the following
# scenario as an error:
#
# An extent is shared between two inodes, as a result of clone/reflink operation,
# and for one of the inodes, lets call it inode A, the extent is referenced
# through a file extent item as a prealloc extent, while for the other inode,
# call it inode B, the extent is referenced through a regular file extent item,
# that is, it was written to. The goal of this test is to make sure a filesystem
# check operation will not report "odd csum items" errors for the prealloc
# extent at inode A, because this scenario is valid since the extent was written
# through inode B and therefore it is expected to have checksum items in the
# filesystem's checksum btree for that shared extent.
#
# Such scenario can be created with the following steps for example:
#
# mkfs.btrfs -f /dev/sdb
# mount /dev/sdb /mnt
#
# touch /mnt/foo
# xfs_io -c "falloc 0 256K" /mnt/foo
# sync
#
# xfs_io -c "pwrite -S 0xab 0 256K" /mnt/foo
# touch /mnt/bar
# xfs_io -c "reflink /mnt/foo 0 0 256K" /mnt/bar
# xfs_io -c "fsync" /mnt/bar
#
# <power fail>
# mount /dev/sdb /mnt
# umount /mnt

source "$TEST_TOP/common"

check_prereq btrfs

check_image() {
	run_check "$TOP/btrfs" check "$1"
}

check_all_images
