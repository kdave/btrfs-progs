#!/bin/bash
#
# qgroup show behaviour when quotas are not enabled

source "$TEST_TOP/common"

check_prereq mkfs.btrfs
check_prereq btrfs

setup_root_helper
prepare_test_dev

run_check_mkfs_test_dev
run_check_mount_test_dev
run_mayfail "$TOP/btrfs" qgroup show "$TEST_MNT"
run_mayfail $SUDO_HELPER "$TOP/btrfs" qgroup show "$TEST_MNT"
run_check $SUDO_HELPER "$TOP/btrfs" quota enable "$TEST_MNT"
run_mayfail "$TOP/btrfs" qgroup show "$TEST_MNT"
run_check $SUDO_HELPER "$TOP/btrfs" qgroup show "$TEST_MNT"
run_check $SUDO_HELPER "$TOP/btrfs" quota disable "$TEST_MNT"
run_check_umount_test_dev
