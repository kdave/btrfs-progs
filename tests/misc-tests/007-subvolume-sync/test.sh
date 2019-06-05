#!/bin/bash
# test btrfs subvolume run normally with more than one subvolume
#
# - btrfs subvolume must not loop indefinitely
# - btrfs subvolume return 0 in normal case

source "$TEST_TOP/common"

check_prereq mkfs.btrfs
check_prereq btrfs

setup_root_helper
prepare_test_dev

run_check_mkfs_test_dev
run_check_mount_test_dev

# to check following thing in both 1 and multiple subvolume case:
# 1: is subvolume sync loop indefinitely
# 2: is return value right
#
run_check $SUDO_HELPER "$TOP/btrfs" subvolume create "$TEST_MNT"/mysubvol1
run_check $SUDO_HELPER "$TOP/btrfs" subvolume create "$TEST_MNT"/mysubvol2
run_check $SUDO_HELPER "$TOP/btrfs" subvolume delete "$TEST_MNT"/mysubvol1
run_check $SUDO_HELPER "$TOP/btrfs" subvolume delete "$TEST_MNT"/mysubvol2
run_check $SUDO_HELPER "$TOP/btrfs" subvolume sync "$TEST_MNT"

run_check $SUDO_HELPER "$TOP/btrfs" subvolume create "$TEST_MNT"/mysubvol
run_check $SUDO_HELPER "$TOP/btrfs" subvolume delete "$TEST_MNT"/mysubvol
run_check $SUDO_HELPER "$TOP/btrfs" subvolume sync "$TEST_MNT"

run_check_umount_test_dev
