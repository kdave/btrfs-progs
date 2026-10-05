#!/bin/bash
#
# Make sure "btrfs check" can properly handle a quota tree whose level
# is larger than 0.

source "$TEST_TOP/common" || exit

check_prereq btrfs
check_prereq mkfs.btrfs

setup_root_helper
prepare_test_dev

run_check_mkfs_test_dev -O quota
run_check_mount_test_dev
# Create 512 subvolumes, which should cause at least 2 leaves even for 64K nodesize.
for ((i = 0; i < 512; i++)); do
	run_check $SUDO_HELPER "$TOP/btrfs" subvolume create "$TEST_MNT/subv_$i"
done

# Create qgroup 1/1 and add 0/5 to it, which should cause unpatched
# btrfs-check to only load the last leaf for relation items, without
# loading the involved qgroups.
run_check $SUDO_HELPER "$TOP/btrfs" qgroup create 1/1 "$TEST_MNT"
run_check $SUDO_HELPER "$TOP/btrfs" qgroup assign 0/5 1/1 "$TEST_MNT"
run_check_umount_test_dev

run_check $SUDO_HELPER "$TOP/btrfs" check "$TEST_DEV"
