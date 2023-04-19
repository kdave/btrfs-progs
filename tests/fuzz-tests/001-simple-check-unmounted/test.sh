#!/bin/bash

# iterate over all fuzzed images and run 'btrfs check'

source "$TEST_TOP/common" || exit

check_prereq btrfs

setup_root_helper

# redefine the one provided by common
check_image() {
	run_mayfail "$TOP/btrfs" check "$1"
}

check_all_images "$TEST_TOP/fuzz-tests/images"

exit 0
