#!/bin/bash

# iterate over all fuzzed images and run 'btrfs-image'

source "$TEST_TOP/common" || exit

check_prereq btrfs-image

setup_root_helper

# redefine the one provided by common
check_image() {
	truncate -s0 target
	run_mayfail "$TOP/btrfs-image" "$1" target
	truncate -s0 target
}

check_all_images "$TEST_TOP/fuzz-tests/images"

rm -- target

exit 0
