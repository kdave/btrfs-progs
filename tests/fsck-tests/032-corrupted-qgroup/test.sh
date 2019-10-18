#!/bin/bash
# Check if btrfs check can handle valid orphan items.
# Orphan item is a marker for deleted inodes that were open at the time of
# deletion.  Orphan inode/root is not referenced and will have an orphan
# item, which should not be reported as error.

source "$TEST_TOP/common"

check_prereq btrfs

check_image() {
	run_mustfail "btrfs check failed to detect qgroup corruption" \
		     "$TOP/btrfs" check "$1"
	# Above command can fail due to other bugs, so add extra check to
	# ensure we can fix qgroup without problems.
	run_check "$TOP/btrfs" check --repair --force "$1"
}

check_all_images
