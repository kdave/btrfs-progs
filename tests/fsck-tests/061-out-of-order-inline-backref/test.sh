#!/bin/bash
#
# Verify that check can detect out-of-order inline backref items.
#
# There is a report that one ntfs2btrfs generated some out-of-order inline
# backref items, while btrfs-check failed to detect it at all.
#
# Make sure btrfs check can at least detect such error.

source "$TEST_TOP/common"

check_prereq btrfs

check_image() {
	run_mustfail "out-of-order inline backref items not detected" \
		"$TOP/btrfs" check "$1"
}

check_all_images
