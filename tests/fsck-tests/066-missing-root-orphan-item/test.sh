#!/bin/bash
#
# Verify that check can report missing orphan root itemm as an error

source "$TEST_TOP/common" || exit

check_prereq btrfs

check_image() {
	run_mustfail "missing root orphan item not reported as an error" \
		"$TOP/btrfs" check "$1"
}

check_all_images
