#!/bin/bash
#
# Verify that check fails on a key whose type does not belong in the tree it
# was found in. It used to print the key, skip it and report no error.

source "$TEST_TOP/common" || exit

check_prereq btrfs

check_image() {
	# The image has a DEV_EXTENT item at the end of the extent tree leaf,
	# well formed so that the tree checker lets it through.
	run_mustfail "invalid key type not reported as an error" \
		"$TOP/btrfs" check "$1"
}

check_all_images
