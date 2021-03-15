#!/bin/bash
# Crafted image with needs_recovery incompat bit feature set, convert must
# refuse to convert such image

source "$TEST_TOP/common"

check_prereq btrfs-convert
check_prereq btrfs

setup_root_helper
prepare_test_dev

# Override common function
check_image() {
	local features

	TEST_DEV="$1"
	features=$(run_check_stdout dumpe2fs "$TEST_DEV" | grep 'Filesystem features')
	if ! echo "$features" | grep -q 'needs_recovery'; then
		_fail "image does not have the needs_recovery bit set"
	fi
	run_mustfail "convert worked on unclean image" \
		"$TOP/btrfs-convert" "$TEST_DEV"
	rm -f "$TEST_DEV"
}

check_all_images
