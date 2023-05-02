#!/bin/bash
# Test btrfstune --convert-to-free-space-tree option

source "$TEST_TOP/common" || exit
source "$TEST_TOP/common.convert" || exit

check_prereq mkfs.btrfs
check_prereq btrfstune
check_prereq btrfs

setup_root_helper
prepare_test_dev

run_check_mkfs_test_dev -O ^free-space-tree
run_check_mount_test_dev
populate_fs
run_check_umount_test_dev

run_check $SUDO_HELPER "$TOP/btrfstune" --convert-to-free-space-tree "$TEST_DEV"

run_check "$TOP/btrfs" check "$TEST_DEV"
