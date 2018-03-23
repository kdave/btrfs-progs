#!/bin/bash

source "$TEST_TOP/common"

setup_root_helper
check_prereq btrfs

# redefine the one provided by common
check_image() {
	run_mayfail "$TOP/btrfs" inspect-internal dump-tree "$1"
}

check_all_images "$TEST_TOP/fuzz-tests/images"

exit 0
