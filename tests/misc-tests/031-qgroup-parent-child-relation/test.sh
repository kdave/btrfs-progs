#!/bin/bash
# Test that btrfs 'qgroup show' outputs the correct parent-child qgroup relation

source "$TEST_TOP/common"

check_prereq mkfs.btrfs
check_prereq btrfs

setup_root_helper
prepare_test_dev

run_check_mkfs_test_dev
run_check_mount_test_dev

run_check $SUDO_HELPER "$TOP/btrfs" quota enable "$TEST_MNT"
run_check $SUDO_HELPER "$TOP/btrfs" qgroup create 1/0 "$TEST_MNT"
run_check $SUDO_HELPER "$TOP/btrfs" qgroup assign 0/5 1/0 "$TEST_MNT"
run_check $SUDO_HELPER "$TOP/btrfs" quota rescan -w "$TEST_MNT"

run_check_stdout $SUDO_HELPER "$TOP/btrfs" qgroup show --sort=-qgroupid \
	-p "$TEST_MNT" | grep -q "1/0" \
	|| _fail "parent qgroup check failed, please check the log"
run_check_stdout $SUDO_HELPER "$TOP/btrfs" qgroup show --sort=qgroupid \
	-c "$TEST_MNT" | grep -q "0/5" \
	|| _fail "child qgroup check failed, please check the log"

run_check_umount_test_dev "$TEST_MNT"
