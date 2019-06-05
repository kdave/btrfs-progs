#!/bin/bash
#
# test receiving stream that's not valid, simple cases

source "$TEST_TOP/common"

check_prereq mkfs.btrfs
check_prereq btrfs

setup_root_helper
prepare_test_dev

run_check_mkfs_test_dev
run_check_mount_test_dev

echo -n '' | run_mayfail $SUDO_HELPER "$TOP/btrfs" receive "$TEST_MNT" &&
	_fail "unexpected: received empty stream"

echo -n '1' | run_mayfail $SUDO_HELPER "$TOP/btrfs" receive "$TEST_MNT" &&
	_fail "unexpected: received stream with shrot and corrupted header"

echo -n '12345678901234567' | run_mayfail $SUDO_HELPER "$TOP/btrfs" receive "$TEST_MNT" &&
	_fail "unexpected: received stream with corrupted header"

run_check_umount_test_dev
