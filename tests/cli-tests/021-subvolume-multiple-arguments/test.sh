#!/bin/bash
# Create subvolume with multiple destinations

source "$TEST_TOP/common" || exit

setup_root_helper
prepare_test_dev

run_check_mkfs_test_dev
run_check_mount_test_dev

# Create one invalid subvolume with 2 valid ones.
# The command should return 1 but the 2 valid ones should be created.
run_mustfail "should report error for any failed subvolume creation" \
	$SUDO_HELPER "$TOP/btrfs" subvolume create \
	"$TEST_MNT/non-exist-dir/subv0" \
	"$TEST_MNT/subv1" \
	"$TEST_MNT/subv2"

run_check $SUDO_HELPER stat "$TEST_MNT/subv1"
run_check $SUDO_HELPER stat "$TEST_MNT/subv2"

# Create multiple subvolumes with parent
run_check $SUDO_HELPER "$TOP/btrfs" subvolume create -p \
	"$TEST_MNT/non-exist-dir/subv0" \
	"$TEST_MNT/subv1/subv3" \
	"$TEST_MNT/subv4" \

run_check $SUDO_HELPER stat "$TEST_MNT/non-exist-dir/subv0"
run_check $SUDO_HELPER stat "$TEST_MNT/subv1/subv3"
run_check $SUDO_HELPER stat "$TEST_MNT/subv4"

run_check $SUDO_HELPER "$TOP/btrfs" subvolume create -p "$TEST_MNT/dir3/dir1/./..//.///subv3//////"
run_check $SUDO_HELPER stat "$TEST_MNT/dir3/dir1"
run_check $SUDO_HELPER stat "$TEST_MNT/dir3/subv3"
run_check find "$TEST_MNT" -ls
run_check_umount_test_dev
