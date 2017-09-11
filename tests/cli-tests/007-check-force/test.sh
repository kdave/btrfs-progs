#!/bin/bash
#
# test 'btrfs check --force' on a mounted filesystem

source "$TOP/tests/common"

check_prereq mkfs.btrfs
check_prereq btrfs

setup_root_helper
prepare_test_dev

run_check "$TOP/mkfs.btrfs" -f "$TEST_DEV"
run_check_mount_test_dev
run_mustfail "checking mounted filesystem without --force" \
	$SUDO_HELPER "$TOP/btrfs" check "$TEST_DEV"
run_check $SUDO_HELPER "$TOP/btrfs" check --force "$TEST_DEV"
run_mustfail "checking mounted filesystem with --force --repair" \
	$SUDO_HELPER "$TOP/btrfs" check --force --repair "$TEST_DEV"
run_check_umount_test_dev
run_check $SUDO_HELPER "$TOP/btrfs" check "$TEST_DEV"
run_check $SUDO_HELPER "$TOP/btrfs" check --force "$TEST_DEV"
run_mustfail "--force --repair on unmounted filesystem" \
	$SUDO_HELPER "$TOP/btrfs" check --force --repair "$TEST_DEV"
