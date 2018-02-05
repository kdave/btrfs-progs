#!/bin/bash
# To check if btrfs check can handle valid orphan items.
# Orphan item is a marker for deleted inodes that were open at the time of
# deletion.  # Orphan inode/root will is not referenced and will have an orphan
# item, which should not be reported as error.

source "$TEST_TOP/common"

check_prereq btrfs

check_image() {
	run_check "$TOP/btrfs" check "$1"
}

check_all_images
