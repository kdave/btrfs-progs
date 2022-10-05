#!/bin/sh
# Test that only toplevel directory self-reference is created

source "$TEST_TOP/common"
source "$TEST_TOP/common.convert"

setup_root_helper
prepare_test_dev

if ! check_kernel_support_reiserfs >/dev/null; then
	_not_run "no reiserfs support"
fi

check_global_prereq mkreiserfs
check_prereq btrfs-convert

run_check $SUDO_HELPER mkreiserfs -q -f -b 4096 "$TEST_DEV"
run_check_mount_test_dev -t reiserfs
run_check $SUDO_HELPER mkdir "$TEST_MNT/a"
run_check $SUDO_HELPER dd if=/dev/urandom of="$TEST_MNT/a/x" bs=1M count=64
run_check $SUDO_HELPER mkdir "$TEST_MNT/a/y"
run_check_umount_test_dev

run_check $SUDO_HELPER "$TOP/btrfs-convert" "$TEST_DEV"
run_check $SUDO_HELPER "$TOP/btrfs" check "$TEST_DEV"
