#!/bin/bash
#
# Verify that btrfs check can detect compressed extent without csum as an error
#
# There is report about nodatasum inode which has compressed extent, and when
# its compressed data is corrupted, decompress screw up the whole kernel.
#
# While btrfs(5) shows that nodatasum will disable data CoW and compression,
# kernel obviously doesn't follow it well. And in above case, lzo problem
# can leads to more serious kernel memory corruption since btrfs completely
# depends its csum to prevent corruption.
#
# So btrfs check should report such compressed extent without csum as error.

source "$TEST_TOP/common"

check_prereq btrfs

check_image() {
	run_mustfail "btrfs fails to detect compressed extent with csum as an error" \
		"$TOP/btrfs" check "$1"
}

check_all_images
