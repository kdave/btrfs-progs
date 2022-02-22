#!/bin/bash
# We could potentially have a directory and it's children with ORPHAN items left
# for them without having been cleaned up.
#
# fsck shouldn't complain about this or attempt to do anything about it, the
# orphan cleanup will do the correct thing.
#
# To create this image I simply modified the kernel to skip doing the
# btrfs_truncate_inode_items() and removing the orphan item at evict time, and
# then rm -rf'ed a directory.

source "$TEST_TOP/common"

check_prereq btrfs

check_image() {
	run_check "$TOP/btrfs" check "$1"
}

check_all_images
