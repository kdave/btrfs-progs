#!/bin/bash
# To check if btrfs check can handle half dropped inodes.
# Such inodes are orphan inodes going through file items evicting.
# During that evicting, btrfs won't bother updating the nbytes of the orphan
# inode as they will soon be removed completely.
#
# Btrfs check should be able to recognize such inodes without giving false
# alerts
#
# The way to reproduce the image:
# - Create a lot of regular file extents for one inode
#   Using direct IO with small block size is the easy method
# - Modify kernel to commit transaction more aggressively
#   Two locations are needed:
#   * btrfs_unlink():
#     To make the ORPHAN item reach disk asap
#   * btrfs_evict_inode():
#     Make the btrfs_end_transaction() call after btrfs_trncate_inode_items()
#     to commit transaction, so we can catch every file item deletion
# - Setup dm-log-writes
#   To catch every transaction commit
# - Delete the inode
# - Sync the fs
# - Replay the log

source "$TEST_TOP/common"

check_prereq btrfs

check_image() {
	run_check "$TOP/btrfs" check "$1"
}

check_all_images
