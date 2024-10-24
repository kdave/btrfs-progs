#!/bin/bash
#
# Verify that check doesn't cause false alerts on various valid log trees

source "$TEST_TOP/common" || exit

check_prereq btrfs

check_image() {
	run_check "$TOP/btrfs" check "$1"
}

check_all_images
