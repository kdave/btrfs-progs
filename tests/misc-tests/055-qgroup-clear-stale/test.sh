#!/bin/bash
#
# Clear stale qgroups

source "$TEST_TOP/common" || exit

check_prereq mkfs.btrfs
check_prereq btrfs

setup_root_helper
prepare_test_dev

run_check_mkfs_test_dev
run_check_mount_test_dev
run_check $SUDO_HELPER "$TOP/btrfs" quota enable "$TEST_MNT"
run_check $SUDO_HELPER "$TOP/btrfs" subvolume create "$TEST_MNT/subv1"
run_check $SUDO_HELPER "$TOP/btrfs" qgroup show "$TEST_MNT"
rootid=$(run_check_stdout "$TOP/btrfs" inspect-internal rootid "$TEST_MNT/subv1")
run_check $SUDO_HELPER "$TOP/btrfs" subvolume delete "$TEST_MNT/subv1"
run_check $SUDO_HELPER "$TOP/btrfs" qgroup show "$TEST_MNT"

# Subvolume under deletion won't be deleted and "btrfs qgroup clear-stale"
# would detect it and not report an error.
# So here we have to wait for the subvolume deletion.
run_check $SUDO_HELPER "$TOP/btrfs" subvolume sync "$TEST_MNT" "$rootid"
run_check $SUDO_HELPER "$TOP/btrfs" qgroup show "$TEST_MNT"
# After cleaning the subvolume it must pass
run_check $SUDO_HELPER "$TOP/btrfs" qgroup clear-stale "$TEST_MNT"

list=$(run_check_stdout $SUDO_HELPER "$TOP/btrfs" qgroup show "$TEST_MNT")
if echo "$list" | grep -q "0/$rootid"; then
	_fail "stale qgroups not deleted"
fi
if echo "$list" | grep -q "<stale>"; then
	_fail "stale qgroups not deleted"
fi
run_check_umount_test_dev
