#!/bin/bash
# Verify that default subvolume specified as 0 will be resolved as the toplevel
# one and not the containing subvolume of the given path

source "$TEST_TOP/common"

setup_root_helper
prepare_test_dev

check_prereq btrfs
check_prereq mkfs.btrfs

run_check_mkfs_test_dev
run_check_mount_test_dev

run_check $SUDO_HELPER "$TOP/btrfs" subvolume create "$TEST_MNT/subv1"
run_check $SUDO_HELPER "$TOP/btrfs" subvolume create "$TEST_MNT/subv1/subv2"

idtop=$(run_check_stdout $SUDO_HELPER "$TOP/btrfs" inspect-internal rootid "$TEST_MNT")
id1=$(run_check_stdout $SUDO_HELPER "$TOP/btrfs" inspect-internal rootid "$TEST_MNT/subv1")
id2=$(run_check_stdout $SUDO_HELPER "$TOP/btrfs" inspect-internal rootid "$TEST_MNT/subv1/subv2")

run_check $SUDO_HELPER "$TOP/btrfs" subvolume set-default "$id1" "$TEST_MNT"
default=$(run_check_stdout $SUDO_HELPER "$TOP/btrfs" subvolume get-default "$TEST_MNT" | awk '{print $2}')

if [ "$default" != "$id1" ]; then
	_fail "setting default subvolume to $id1 did not work, found $default"
fi

run_check_umount_test_dev

# Mount subv2 into $TEST_MNT, while id1 is default
run_check_mount_test_dev -o subvolid="$id2"
# Change default back to toplevel fs tree but point to the non-default id2
run_check_stdout $SUDO_HELPER "$TOP/btrfs" subvolume set-default 0 "$TEST_MNT"
default=$(run_check_stdout $SUDO_HELPER "$TOP/btrfs" subvolume get-default "$TEST_MNT" | awk '{print $2}')

if [ "$default" != 5 ]; then
	_fail "toplevel subvolume not set, found $default"
fi

run_check_umount_test_dev
