#!/bin/bash
#
# Verify that check can detect missing log csum items.

source "$TEST_TOP/common" || exit

check_prereq btrfs

check_image() {
	if [[ "$1" == "./default.img.restored" ]]; then
		run_mustfail "missing log csum items not detected" \
			"$TOP/btrfs" check "$1"
	else
		run_check "$TOP/btrfs" check "$1"
	fi
}

check_all_images
