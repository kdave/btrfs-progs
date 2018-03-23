#!/bin/bash
# Make sure btrfs check won't report any false alerts for valid image with
# reloc tree.
#
# Also due to the short life span of reloc tree, save the as dump example for
# later usage.

source "$TEST_TOP/common"

check_prereq btrfs

check_image() {
	run_check "$TOP/btrfs" check "$1"
}

check_all_images
