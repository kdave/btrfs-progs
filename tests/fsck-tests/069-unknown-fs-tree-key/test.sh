#!/bin/bash
#
# Verify that check can report unknown key types in subvolume trees

source "$TEST_TOP/common" || exit

check_prereq btrfs

check_image() {
	run_mustfail "unknown keys in subvolume trees not reported as error" \
		"$TOP/btrfs" check "$1"
}

check_all_images
