#!/bin/bash

# iterate over all fuzzed images and run 'btrfs check', try various options to
# get more code coverage

source $TOP/tests/common

setup_root_helper
check_prereq btrfs

# redefine the one provided by common
check_image() {
	local image

	image=$1
	run_mayfail $TOP/btrfs check -s 1 "$image"
	run_mayfail $TOP/btrfs check --init-csum-tree "$image"
	run_mayfail $TOP/btrfs check --init-extent-tree "$image"
	run_mayfail $TOP/btrfs check --check-data-csum "$image"
	run_mayfail $TOP/btrfs check --subvol-extents "$image"
	run_mayfail $TOP/btrfs check --repair "$image"
}

check_all_images $TOP/tests/fuzz-tests/images

exit 0
