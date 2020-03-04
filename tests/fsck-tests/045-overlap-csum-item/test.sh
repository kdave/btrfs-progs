#!/bin/bash
#
# Verify that check can detect overlapped csum items
#
# There is one report of overlap csum items, causing tree-checker to reject
# the csum tree.
#
# Make sure btrfs check can at least detect such error

source "$TEST_TOP/common"

check_prereq btrfs

check_image() {
	run_mustfail "overlapping csum item not detected" \
		"$TOP/btrfs" check "$1"
}

check_all_images
