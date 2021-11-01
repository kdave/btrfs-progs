#!/bin/bash
# Look for warning about read-write subvolume with received_uuid set, on a crafted
# image

source "$TEST_TOP/common"

setup_root_helper
prepare_test_dev

ORIG_TEST_DEV="$TEST_DEV"
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

TEST_DEV="$ORIG_TEST_DEV"
run_check_mkfs_test_dev
run_check_mount_test_dev
run_check $SUDO_HELPER "$TOP/btrfs" subvolume create "$TEST_MNT/subvol1"
if run_check_stdout $SUDO_HELPER "$TOP/btrfs" subvolume show "$TEST_MNT/subvol1" |
     grep -q "WARNING.*received_uuid"; then
	_fail "unexpected warning"
fi
run_check $SUDO_HELPER "$TOP/btrfs" subvolume snapshot -r "$TEST_MNT/subvol1" "$TEST_MNT/snap1"
if run_check_stdout $SUDO_HELPER "$TOP/btrfs" subvolume show "$TEST_MNT/snap1" |
     grep -q "WARNING.*received_uuid"; then
	_fail "unexpected warning"
fi
run_check $SUDO_HELPER mkdir "$TEST_MNT/recv"
touch send.stream
chmod a+w send.stream
run_check $SUDO_HELPER "$TOP/btrfs" send -f send.stream "$TEST_MNT/snap1"
run_check $SUDO_HELPER "$TOP/btrfs" receive -f send.stream -m "$TEST_MNT" "$TEST_MNT/recv"
if run_check_stdout $SUDO_HELPER "$TOP/btrfs" subvolume show "$TEST_MNT/recv/snap1" |
     grep -q "WARNING.*received_uuid"; then
	_fail "unexpected warning"
fi
run_check_umount_test_dev

rm -- send.stream
