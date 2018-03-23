#!/bin/bash
# test various sectorsize and node size combinations
# including valid and invalid ones
# only do mkfs and fsck check, no mounting as
# sub/multi-pagesize is not supported yet

source "$TEST_TOP/common"

check_prereq mkfs.btrfs
check_prereq btrfs

prepare_test_dev

# disable mixed bg to avoid sectorsize == nodesize check
features="^mixed-bg"

# caller need to check whether the combination is valid
do_test()
{
	sectorsize=$1
	nodesize=$2
	run_mayfail "$TOP/mkfs.btrfs" -f -O "$features" -n "$nodesize" -s "$sectorsize" \
		"$TEST_DEV"
	ret=$?
	if [ "$ret" == 0 ]; then
		run_check "$TOP/btrfs" check "$TEST_DEV"
	fi
	return "$ret"
}

# Invalid: Unaligned sectorsize and nodesize
do_test 8191 8191 && _fail

# Invalid: Aligned sectorsize with unaligned nodesize
do_test 4k 16385 && _fail

# Invalid: Unaligned sectorsize with aligned nodesize
do_test 8191 16k && _fail

# Valid: Aligned sectorsize and nodesize
do_test 4k 16k || _fail

# Invalid: Sectorsize larger than nodesize
do_test 8k 4k && _fail

# Invalid: too large nodesize
do_test 16k 128k && _fail

# Valid: large sectorsize
do_test 64k 64k || _fail
