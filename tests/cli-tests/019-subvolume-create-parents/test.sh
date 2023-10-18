#!/bin/bash
# Create subvolume with aid of option -p

source "$TEST_TOP/common" || exit

setup_root_helper
prepare_test_dev

run_check_mkfs_test_dev
run_check_mount_test_dev
# Subvolume in toplevel directory
run_check $SUDO_HELPER "$TOP/btrfs" subvolume create "$TEST_MNT/subv1"
run_check $SUDO_HELPER stat "$TEST_MNT/subv1"
run_check $SUDO_HELPER "$TOP/btrfs" subvolume create -p "$TEST_MNT/subv1-p"
run_check $SUDO_HELPER stat "$TEST_MNT/subv1-p"
run_mustfail "was able to create subvolume without an intermediate directory" \
	$SUDO_HELPER "$TOP/btrfs" subvolume create "$TEST_MNT/dir1/subv1"
run_check $SUDO_HELPER "$TOP/btrfs" subvolume create -p "$TEST_MNT/dir1/subv1"
run_check $SUDO_HELPER stat "$TEST_MNT/dir1/subv1"
# Repeat last, dir1 exists
run_check $SUDO_HELPER "$TOP/btrfs" subvolume create -p "$TEST_MNT/dir1/subv2"
run_check $SUDO_HELPER stat "$TEST_MNT/dir1/subv2"
# Create inside one of the subvolumes
run_check $SUDO_HELPER "$TOP/btrfs" subvolume create -p "$TEST_MNT/dir1/subv1/dir2/dir3/dir4/subv2"
run_check $SUDO_HELPER stat "$TEST_MNT/dir1/subv1/dir2/dir3/dir4/subv2"
# Unclean path
run_check $SUDO_HELPER "$TOP/btrfs" subvolume create -p "$TEST_MNT/dir2///////subv1//////"
run_check $SUDO_HELPER stat "$TEST_MNT/dir2/subv1"
# Uncleal path, undo dir3, create under dir2
run_check $SUDO_HELPER "$TOP/btrfs" subvolume create -p "$TEST_MNT/dir3/dir1/./..//.///subv3//////"
run_check $SUDO_HELPER stat "$TEST_MNT/dir3/dir1"
run_check $SUDO_HELPER stat "$TEST_MNT/dir3/subv3"
run_check find "$TEST_MNT" -ls
run_check_umount_test_dev
