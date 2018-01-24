#!/bin/bash
# test if the reported minimal size of mkfs.btrfs is valid

source "$TOP/tests/common"

check_prereq mkfs.btrfs
check_prereq btrfs

setup_root_helper

do_test()
{
	# 1M should be small enough to reliably fail, we use the output to get
	# the minimal device size for the given option combination
	prepare_test_dev 1M
	output=$(run_mustfail_stdout "mkfs.btrfs for small image" \
		 "$TOP/mkfs.btrfs" -f $@ "$TEST_DEV")
	good_size=$(echo "$output" | grep -oP "(?<=is )\d+")

	prepare_test_dev "$good_size"
	echo "Minimal device size is $good_size" >> "$RESULTS"
	run_check $TOP/mkfs.btrfs -f $@ "$TEST_DEV"
	run_check_mount_test_dev
	run_check_umount_test_dev
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
