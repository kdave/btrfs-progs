#!/bin/bash
#
# Verify that check can report duplicate filename as an error

source "$TEST_TOP/common" || exit

check_prereq btrfs

check_image() {
	run_mustfail "duplicate filename not reported as error" \
		"$TOP/btrfs" check "$1"
}

check_all_images
