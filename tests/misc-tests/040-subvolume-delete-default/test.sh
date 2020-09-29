#!/bin/bash
# Detect if a default subvolume is being deleted

source "$TEST_TOP/common"

check_prereq mkfs.btrfs
check_prereq btrfs

prepare_test_dev

run_check_mkfs_test_dev
run_check_mount_test_dev
default=$(run_check_stdout $SUDO_HELPER "$TOP/btrfs" subvolume get-default "$TEST_MNT")

for i in `seq 10`; do
	run_check $SUDO_HELPER "$TOP/btrfs" subvolume create "$TEST_MNT/subv$i"
done
rootid=$(run_check_stdout $SUDO_HELPER "$TOP/btrfs" inspect-internal rootid "$TEST_MNT/subv5")
run_check $SUDO_HELPER "$TOP/btrfs" subvolume set-default "$rootid" "$TEST_MNT"
default=$(run_check_stdout $SUDO_HELPER "$TOP/btrfs" subvolume get-default "$TEST_MNT")

# Delete all subvolumes, it will continue after individual errors and will return 1
run_mustfail "deleting default subvolume by path succeeded" \
	$SUDO_HELPER "$TOP/btrfs" subvolume delete "$TEST_MNT/"subv*

run_mustfail "deleting default subvolume by id succeeded" \
	$SUDO_HELPER "$TOP/btrfs" subvolume delete --subvolid "$rootid" "$TEST_MNT"

run_check_umount_test_dev
