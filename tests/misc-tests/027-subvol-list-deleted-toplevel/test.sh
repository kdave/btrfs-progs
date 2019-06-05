#!/bin/bash
# check that the toplevel subvolume is not listed as regular or deleted

source "$TEST_TOP/common"

check_prereq mkfs.btrfs
check_prereq btrfs

prepare_test_dev

run_check_mkfs_test_dev
run_check_mount_test_dev
run_check_stdout $SUDO_HELPER "$TOP/btrfs" subvolume list "$TEST_MNT" |
	grep -i -q "id 5" && _fail "found toplevel among regular"
run_check_stdout $SUDO_HELPER "$TOP/btrfs" subvolume list -d "$TEST_MNT" |
	grep -i -q "id 5.*DELETED" && _fail "found toplevel among deleted"

run_check_umount_test_dev
