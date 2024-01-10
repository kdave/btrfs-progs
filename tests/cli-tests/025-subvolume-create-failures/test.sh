#!/bin/bash
# Create subvolume failure cases to make sure the return value is correct

source "$TEST_TOP/common" || exit

setup_root_helper
prepare_test_dev

run_check_mkfs_test_dev
run_check_mount_test_dev

# Create one subvolume and one file as place holder for later subvolume
# creation to fail.
run_check $SUDO_HELPER "$TOP/btrfs" subvolume create "$TEST_MNT/subv1"
run_check $SUDO_HELPER touch "$TEST_MNT/subv2"

# Using existing path to create a subvolume must fail
run_mustfail "should report error when target path already exists" \
	$SUDO_HELPER "$TOP/btrfs" subvolume create "$TEST_MNT/subv1"

run_mustfail "should report error when target path already exists" \
	$SUDO_HELPER "$TOP/btrfs" subvolume create "$TEST_MNT/subv2"

# Using multiple subvolumes in one create go, the good one "subv3" should be created
run_mustfail "should report error when target path already exists" \
	$SUDO_HELPER "$TOP/btrfs" subvolume create \
	"$TEST_MNT/subv1" "$TEST_MNT/subv2" "$TEST_MNT/subv3"

run_check $SUDO_HELPER stat "$TEST_MNT/subv3"

run_check_umount_test_dev
