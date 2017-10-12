#!/bin/bash
# test if the reported minimal size of mkfs.btrfs is valid

source $TOP/tests/common

check_prereq mkfs.btrfs
check_prereq btrfs

setup_root_helper

pagesize=$(getconf PAGESIZE)

do_test()
{
	# Well, 1M small enough to fail, we just use the output
	# to get the minimal device size
	prepare_test_dev 1M
	output=$(run_mustfail_stdout "mkfs.btrfs for small image" \
		 $TOP/mkfs.btrfs -f $@ "$TEST_DEV")
	good_size=$(echo $output | grep -oP "(?<=is )\d+")

	prepare_test_dev "$good_size"
	run_check $TOP/mkfs.btrfs -f $@ "$TEST_DEV"
	run_check $SUDO_HELPER mount $TEST_DEV $TEST_MNT
	run_check $SUDO_HELPER umount $TEST_MNT
}

do_test -n 4k	-m single	-d single
do_test -n 4k	-m single	-d dup
do_test -n 4k	-m dup		-d single
do_test -n 4k	-m dup		-d dup

do_test -n 8k	-m single	-d single
do_test -n 8k	-m single	-d dup
do_test -n 8k	-m dup		-d single
do_test -n 8k	-m dup		-d dup

do_test -n 16k	-m single	-d single
do_test -n 16k	-m single	-d dup
do_test -n 16k	-m dup		-d single
do_test -n 16k	-m dup		-d dup

do_test -n 32k	-m single	-d single
do_test -n 32k	-m single	-d dup
do_test -n 32k	-m dup		-d single
do_test -n 32k	-m dup		-d dup

do_test -n 64k	-m single	-d single
do_test -n 64k	-m single	-d dup
do_test -n 64k	-m dup		-d single
do_test -n 64k	-m dup		-d dup
