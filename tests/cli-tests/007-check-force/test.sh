#!/bin/bash
#
# test 'btrfs check --force' on a mounted filesystem

source "$TEST_TOP/common"

check_prereq mkfs.btrfs
check_prereq btrfs

setup_root_helper

# we need to use a real block device, because the check opens the device in
# exclusive mode, that unfortunately behaves differently for direct file
# access and for the real /dev/loop0 device
setup_loopdevs 1
prepare_loopdevs
TEST_DEV=${loopdevs[1]}

run_check_mkfs_test_dev
run_check_mount_test_dev
run_mustfail "checking mounted filesystem without --force" \
	$SUDO_HELPER "$TOP/btrfs" check "$TEST_DEV"
run_check $SUDO_HELPER "$TOP/btrfs" check --force "$TEST_DEV"
run_check $SUDO_HELPER "$TOP/btrfs" check --force --repair "$TEST_DEV"
run_check_umount_test_dev
run_check $SUDO_HELPER "$TOP/btrfs" check "$TEST_DEV"
run_check $SUDO_HELPER "$TOP/btrfs" check --force "$TEST_DEV"

cleanup_loopdevs
