#!/bin/bash
# check how deep does recursive defrag go, currently it has to stop at
# mountpoint and subvolume boundary, ie. only the first file should
# appear in the list of processed files

source "$TEST_TOP/common"

check_prereq mkfs.btrfs
check_prereq btrfs

setup_root_helper
prepare_test_dev

run_check_mkfs_test_dev
run_check_mount_test_dev

run_check $SUDO_HELPER dd if=/dev/zero bs=1M count=10 of="$TEST_MNT"/file1
run_check $SUDO_HELPER "$TOP/btrfs" subvolume create "$TEST_MNT"/subv1
run_check $SUDO_HELPER dd if=/dev/zero bs=1M count=10 of="$TEST_MNT"/subv1/file2
run_check $SUDO_HELPER "$TOP/btrfs" subvolume snapshot "$TEST_MNT"/subv1 "$TEST_MNT"/snap1
run_check $SUDO_HELPER dd if=/dev/zero bs=1M count=10 of="$TEST_MNT"/snap1/file3

run_check $SUDO_HELPER find "$TEST_MNT"
run_check_stdout $SUDO_HELPER "$TOP/btrfs" filesystem defrag -v -r "$TEST_MNT" |
	grep -q 'file[23]' && _fail "recursive defrag went to subvolumes"

run_check_umount_test_dev
