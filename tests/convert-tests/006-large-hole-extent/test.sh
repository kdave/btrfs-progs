#!/bin/bash
# Create a base image with large hole extent, then convert to btrfs,
# check the converted image.
# Check if btrfs-convert can handle such large hole.
# Fast pinpoint regression test. No options combination nor checksum
# verification

source "$TEST_TOP/common"
source "$TEST_TOP/common.convert"

setup_root_helper
prepare_test_dev
check_prereq btrfs-convert
check_global_prereq mke2fs

default_mke2fs="mke2fs -t ext4 -b 4096"
convert_test_preamble '' 'large hole extent test' 16k "$default_mke2fs"
convert_test_prep_fs ext4 $default_mke2fs

run_check $SUDO_HELPER dd if=/dev/zero of="$TEST_MNT/file" bs=1M \
	count=1 seek=1024 > /dev/null 2>&1

run_check_umount_test_dev
convert_test_do_convert 
