#!/bin/bash
# check whether btrfsck can detect running qgroup rescan

source "$TEST_TOP/common"

check_prereq btrfs

check_image() {
	local image

	image=$1
	run_check_stdout "$TOP/btrfs" check "$image" 2>&1 | \
		grep -q "Counts for qgroup id"
	if [ $? -eq 0 ]; then
		_fail "Btrfs check doesn't detect rescan correctly"
	fi
}

check_all_images
