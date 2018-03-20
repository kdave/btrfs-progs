#!/bin/bash
# Check if btrfs-convert refuses to rollback the filesystem, and leave the fs
# and the convert image untouched

source "$TEST_TOP/common"
source "$TEST_TOP/common.convert"

setup_root_helper
prepare_test_dev
check_prereq btrfs-convert
check_global_prereq mke2fs

convert_test_prep_fs ext4 mke2fs -t ext4 -b 4096

# Create a 6K file, which should not be inlined
run_check $SUDO_HELPER dd if=/dev/zero bs=2k count=3 of="$TEST_MNT/file1"

run_check_umount_test_dev

# convert_test_do_convert() will call btrfs check, which should expose any
# invalid inline extent with too large size
convert_test_do_convert
