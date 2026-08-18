#!/bin/bash
#
# Verify that check can detect unexpected file extent items for special inodes
# which should not have any file extent.

source "$TEST_TOP/common" || exit

check_prereq btrfs

check_image() {
	# The image contains an BLK inode, with a file extent.
	# The check should report an error.
	run_mustfail "unexpected file extent not detected" "$TOP/btrfs" check "$1"
}

check_all_images
