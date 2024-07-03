#!/bin/bash
#
# Verify that check can detect missing log csum items.

source "$TEST_TOP/common" || exit

check_prereq btrfs

check_image() {
	run_mustfail "missing log csum items not detected" \
		"$TOP/btrfs" check "$1"
}

check_all_images
