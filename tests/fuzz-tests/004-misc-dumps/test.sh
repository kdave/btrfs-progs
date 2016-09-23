#!/bin/bash

# iterate over all fuzzed images and run various tools, do not expect to repair
# or dump succesfully, must not crash at least

source $TOP/tests/common

setup_root_helper
check_prereq btrfs

# redefine the one provided by common
check_image() {
	local image

	image=$1
	run_mayfail $TOP/btrfs inspect-internal dump-tree "$image"
	run_mayfail $TOP/btrfs inspect-internal dump-super "$image"
	run_mayfail $TOP/btrfs inspect-internal dump-super -Ffa "$image"
	run_mayfail $TOP/btrfs inspect-internal tree-stats "$image"
	run_check cp "$image" "$image".scratch
	run_mayfail $TOP/btrfs rescue super-recover -y -v "$image".scratch
	run_check cp "$image" "$image".scratch
	run_mayfail $TOP/btrfs rescue chunk-recover -y -v "$image".scratch
	run_check cp "$image" "$image".scratch
	run_mayfail $TOP/btrfs rescue zero-log "$image".scratch
	rm -- "$image".scratch
}

check_all_images $TOP/tests/fuzz-tests/images

exit 0
