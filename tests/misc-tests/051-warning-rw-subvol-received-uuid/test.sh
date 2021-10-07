#!/bin/bash
# Look for warning about read-write subvolume with received_uuid set, on a crafted
# image

source "$TEST_TOP/common"

setup_root_helper
prepare_test_dev

TEST_DEV=$(extract_image "subvol-rw-recv.img")
run_check_mount_test_dev
if ! run_check_stdout $SUDO_HELPER "$TOP/btrfs" subvolume show "$TEST_MNT/subvol1" |
     grep -q "WARNING.*received_uuid"; then
	_fail "no warning found"
fi
if run_check_stdout $SUDO_HELPER "$TOP/btrfs" subvolume show "$TEST_MNT/snap1" |
     grep -q "WARNING.*received_uuid"; then
	_fail "unexpected warning"
fi
run_check_umount_test_dev

rm -- "$TEST_DEV"
