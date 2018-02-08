#!/bin/bash
# confirm whether 'btrfs check' supports check ing of a partially dropped
# snapshot

source "$TEST_TOP/common"

check_prereq btrfs

check_image()
{
	local image

	image=$1
	run_check_stdout "$TOP/btrfs" check "$image" 2>&1 |
		grep -q "Errors found in extent allocation tree or chunk allocation"
	if [ $? -eq 0 ]; then
		rm -f "$image"
		_fail "unexpected error occurred when checking $img"
	fi
}

check_all_images
