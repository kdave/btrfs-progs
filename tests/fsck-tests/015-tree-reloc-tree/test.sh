#!/bin/bash
# Make sure btrfs check won't report any false alerts for valid image with
# reloc tree.
#
# Also due to the short life span of reloc tree, save the as dump example for
# later usage.

source "$TEST_TOP/common"

check_prereq btrfs

check_image() {
	local image

	image=$1
	run_check "$TOP/btrfs" check "$image"
}

check_all_images
