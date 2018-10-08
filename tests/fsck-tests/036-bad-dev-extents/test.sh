#!/bin/bash
#
# Due to DUP chunk allocator bugs, we could allocate DUP chunks while its dev
# extents could exist beyond device boundary.
# And since all related items (block group, chunk, device used bytes) are all
# valid, btrfs check won't report any error.
#
# This test case contains hand crafted minimal image, to test if btrfs check
# can detect and report such error.

source "$TEST_TOP/common"

check_prereq btrfs

check_image() {
	run_mustfail "btrfs check failed to detect invalid dev extents" \
		"$TOP/btrfs" check "$1"
}

check_all_images
