#!/bin/bash
# Verify that btrfstune would reject fs with transid mismatch problems

source "$TEST_TOP/common"

check_prereq btrfs-image
check_prereq btrfs
check_prereq btrfstune

# Although we're not checking the image, here we just reuse the infrastructure
check_image() {
	run_mustfail "btrfstune should fail when the image has transid error" \
		  "$TOP/btrfstune" -u "$1"
}

check_all_images
