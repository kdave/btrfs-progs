#!/bin/bash
#
# Due to a bug in --init-extent-tree option, we may create bad generation
# number for data extents.
#
# This test case will ensure btrfs check can at least detect such problem,
# just like kernel tree-checker.

source "$TEST_TOP/common"

check_prereq btrfs

check_image() {
	run_mustfail \
		"btrfs check failed to detect invalid extent item generation" \
		"$TOP/btrfs" check "$1"
}

check_all_images
