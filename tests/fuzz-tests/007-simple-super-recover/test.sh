#!/bin/bash

source $TOP/tests/common

setup_root_helper
check_prereq btrfs

# redefine the one provided by common
check_image() {
	local image

	image=$1
	run_check cp "$image" "$image".scratch
	run_mayfail $TOP/btrfs rescue super-recover -y -v "$image".scratch
	rm -- "$image".scratch
}

check_all_images $TOP/tests/fuzz-tests/images

exit 0
