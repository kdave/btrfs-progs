#!/bin/bash
# Create a base image with large hole extent, then convert to btrfs,
# check the converted image.
# Check if btrfs-convert can handle such large hole.
# Fast pinpoint regression test. No options combination nor checksum
# verification

source "$TEST_TOP/common"
source "$TEST_TOP/common.convert"

if ! check_kernel_support_reiserfs >/dev/null; then
	_not_run "no reiserfs support"
fi

setup_root_helper
prepare_test_dev
check_prereq btrfs-convert
check_global_prereq mkreiserfs

default_mkfs="mkreiserfs -b 4096"
convert_test_preamble '' 'large hole extent test' 16k "$default_mkfs"
convert_test_prep_fs reiserfs $default_mkfs

run_check $SUDO_HELPER dd if=/dev/zero of="$TEST_MNT/file" bs=1M \
	count=1 seek=1024 > /dev/null 2>&1

run_check_umount_test_dev
convert_test_do_convert
