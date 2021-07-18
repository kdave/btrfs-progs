#!/bin/bash
#
# Verify that check can detect overlapped dir with 2 links
#
# There is a report that btrfs-check doesn't report dir with 2 links as
# error, and only get caught by tree-checker.
#
# Make sure btrfs check can at least detect such error.

source "$TEST_TOP/common"

check_prereq btrfs

check_image() {
	run_mustfail "dir with 2 links not detected" \
		"$TOP/btrfs" check "$1"
}

check_all_images
