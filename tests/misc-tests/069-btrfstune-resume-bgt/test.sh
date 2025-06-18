#!/bin/bash
# To check if btrfstune can handle half converted fs.
#
# Due to a bug in the block group items loading code, old block groups items
# in the old extent tree can be skipped, thus causing resume failure.

source "$TEST_TOP/common" || exit

check_prereq btrfs

check_image() {
	run_check "$TOP/btrfstune" --convert-to-block-group-tree "$1"
}

check_all_images
