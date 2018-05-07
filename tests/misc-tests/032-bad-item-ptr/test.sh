#!/bin/bash
#
# Verify that btrfs inspect dump-tree won't segfault on heavily corrupted
# tree leaf
# Issue: #128

source "$TEST_TOP/common"

check_prereq btrfs

check_image() {
	run_check "$TOP/btrfs" inspect-internal dump-tree "$1"
	run_mustfail "btrfs check failed to detect such corruption" \
		  "$TOP/btrfs" check "$1"
}

check_all_images
