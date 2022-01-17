#!/bin/bash
#
# Verify that check can detect invalid metadata backref level.
#
# There is a report that btrfs-check original mode doesn't report invalid
# metadata backref level, and lowmem mode would just crash.
#
# Make sure btrfs check can at least detect such error.

source "$TEST_TOP/common"

check_prereq btrfs

check_image() {
	run_mustfail "invalid metadata backref level not detected" \
		"$TOP/btrfs" check "$1"
}

check_all_images
