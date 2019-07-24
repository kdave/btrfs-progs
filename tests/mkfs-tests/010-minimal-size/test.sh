#!/bin/bash
# test if the reported minimal size of mkfs.btrfs is valid

source "$TEST_TOP/common"

check_prereq mkfs.btrfs
check_prereq btrfs

setup_root_helper

do_test()
{
	# 1M should be small enough to reliably fail, we use the output to get
	# the minimal device size for the given option combination
	prepare_test_dev 1M
	output=$(run_mustfail_stdout "mkfs.btrfs for small image" \
		 "$TOP/mkfs.btrfs" -f "$@" "$TEST_DEV")
	good_size=$(echo "$output" | grep -oP "(?<=is )\d+")

	prepare_test_dev "$good_size"
	_log "Minimal device size is $good_size"
	run_check_mkfs_test_dev "$@"
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

# Temporary: disable the following tests as they fail inside travis but run
# fine otherwise. This is probably caused by kernel version, 4.4 fails and 4.14
# is ok.
#
# root_helper mount -t btrfs -o loop /home/travis/build/kdave/btrfs-progs/tests/test.img /home/travis/build/kdave/btrfs-progs/tests/mnt
# mount: No space left on device
# failed: root_helper mount -t btrfs -o loop /home/travis/build/kdave/btrfs-progs/tests/test.img /home/travis/build/kdave/btrfs-progs/tests/mnt
# test failed for case 010-minimal-size
#
if [ "$TRAVIS" = true ]; then
	exit 0
fi

do_test -n 32k	-m single	-d single
do_test -n 32k	-m single	-d dup
do_test -n 32k	-m dup		-d single
do_test -n 32k	-m dup		-d dup

do_test -n 64k	-m single	-d single
do_test -n 64k	-m single	-d dup
do_test -n 64k	-m dup		-d single
do_test -n 64k	-m dup		-d dup
