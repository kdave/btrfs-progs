#!/bin/bash
# Check if block sizes smaller than 4k expectedly fail to convert

source $TOP/tests/common
source $TOP/tests/common.convert

setup_root_helper
prepare_test_dev 512M
check_prereq btrfs-convert

for bs in 1024 2048; do
	default_mke2fs="mke2fs -t ext4 -b $bs"
	convert_test_preamble '' "unsupported block size $bs" 16k "$default_mke2fs"
	convert_test_prep_fs $default_mke2fs

	run_check $SUDO_HELPER dd if=/dev/zero of=$TEST_MNT/file bs=1M \
		count=1 seek=1024 > /dev/null 2>&1

	run_check_umount_test_dev
	run_mustfail "$bs block converted" $TOP/btrfs-convert $TEST_DEV
done
