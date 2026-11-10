#!/bin/bash
#
# Verify that check fails on a key whose type does not belong in the tree it
# was found in. The original mode used to print the key, skip it and report
# no error, the lowmem mode did not look at the type at all.

source "$TEST_TOP/common" || exit

check_prereq btrfs

check_image() {
	# The image has a copy of the FS_TREE root item in front of the items of
	# the extent tree leaf. The item is well formed so that the tree checker
	# lets it through, only its key type is wrong for the tree.
	run_mustfail "invalid key type not reported as an error" \
		"$TOP/btrfs" check "$1"
}

check_all_images
