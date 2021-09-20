#!/bin/bash
# Test parsing of option 'defrag -c'

source "$TEST_TOP/common"

setup_root_helper
prepare_test_dev

run_check_mkfs_test_dev
run_check_mount_test_dev
run_check $SUDO_HELPER dd if=/dev/zero of="$TEST_MNT"/file bs=1M count=1
run_check $SUDO_HELPER "$TOP/btrfs" filesystem defrag -v -c "$TEST_MNT/file"
run_check $SUDO_HELPER "$TOP/btrfs" filesystem defrag -v -clzo "$TEST_MNT/file"
run_check $SUDO_HELPER "$TOP/btrfs" filesystem defrag -v -czlib "$TEST_MNT/file"
run_check $SUDO_HELPER "$TOP/btrfs" filesystem defrag -v -czstd "$TEST_MNT/file"
run_check_umount_test_dev
