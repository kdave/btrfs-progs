#!/bin/bash
#
# Verify that check can handle btrfs with fsverity features properly

source "$TEST_TOP/common" || exit

check_prereq btrfs

check_image() {
	# The image contains verity data in the fs tree.
	# The check should not report verity items as errors.
	run_check "$TOP/btrfs" check "$1"
}

check_all_images
