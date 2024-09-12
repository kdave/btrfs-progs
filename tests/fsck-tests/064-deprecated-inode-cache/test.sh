#!/bin/bash
#
# Verify that check can report deprecated inode cache as an error

source "$TEST_TOP/common" || exit

check_prereq btrfs

check_image() {
	run_mustfail "deprecated inode cache not reported as an error" \
		"$TOP/btrfs" check "$1"
}

check_all_images
