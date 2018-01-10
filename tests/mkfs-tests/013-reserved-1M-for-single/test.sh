#!/bin/bash
# Test if "-m single" or "--mixed" can cause dev extent to use the reserved 1M
# range
#
# Other profiles will cause mkfs.btrfs to allocate new meta/sys chunks
# using btrfs_alloc_chunk() which won't use the 0~1M range, so other profiles
# are safe.

source "$TOP/tests/common"

check_prereq mkfs.btrfs
check_prereq btrfs

prepare_test_dev

do_one_test ()
{
	run_check "$TOP/mkfs.btrfs" -f "$@" "$TEST_DEV"

	# Use dev-extent tree to find first device extent
	first_dev_extent=$(run_check_stdout "$TOP/btrfs" inspect-internal \
		dump-tree -t device "$TEST_DEV" | \
		grep -oP '(?<=DEV_EXTENT )[[:digit:]]*' | head -n1)

	if [ -z $first_dev_extent ]; then
		_fail "failed to get first device extent"
	fi

	echo "first dev extent starts at $first_dev_extent" >> "$RESULTS"
	echo "reserved range is [0, $(( 1024 * 1024)))" >> "$RESULTS"
	# First device extent should not start below 1M
	if [ $first_dev_extent -lt $(( 1024 * 1024 )) ]; then
		_fail "first device extent occupies reserved 0~1M range"
	fi
}

do_one_test --mixed
do_one_test -m single
