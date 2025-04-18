#!/bin/bash
# Check if block sizes smaller than 4k expectedly fail to convert

source "$TEST_TOP/common" || exit
source "$TEST_TOP/common.convert" || exit

check_prereq btrfs-convert
check_global_prereq mke2fs

setup_root_helper
prepare_test_dev

for bs in 1024 2048; do
	default_mke2fs="mke2fs -t ext4 -b $bs"
	convert_test_preamble '' "unsupported block size $bs" 16k "$default_mke2fs"
	convert_test_prep_fs ext4 $default_mke2fs

	run_check $SUDO_HELPER dd if=/dev/zero of="$TEST_MNT/file" bs=1M \
		count=1 seek=1024 > /dev/null 2>&1

	run_check_umount_test_dev
	if _test_config "EXPERIMENTAL" && [ "$bs" == '2048' ]; then
		# Experimental support for mkfs.btrfs --sectorsize 2048
		continue
	fi
	run_mustfail "$bs block converted" "$TOP/btrfs-convert" "$TEST_DEV"
done
