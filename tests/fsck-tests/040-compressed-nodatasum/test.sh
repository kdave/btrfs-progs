#!/bin/bash
#
# Verify that check can detect compressed extent without csum as an error
#
# There is report about nodatasum inode which has compressed extent, and when
# its compressed data is corrupted, decompress screws up the whole kernel.
#
# While btrfs(5) shows that nodatasum will disable data CoW and compression,
# there was bug in kernel that allowed this combination.  And in above case,
# lzo problem can lead to more serious kernel memory corruption since btrfs
# completely depends its csum to prevent corruption.
#
# So btrfs check should report such compressed extent without csum as error.

source "$TEST_TOP/common"

check_prereq btrfs

check_image() {
	run_mustfail "compressed extent with csum not detected" \
		"$TOP/btrfs" check "$1"
}

check_all_images
