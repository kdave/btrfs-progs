#!/bin/bash
#
# btrfs fi du should handle empty subvolumes (with ino == 2)

source "$TEST_TOP/common"

check_prereq mkfs.btrfs
check_prereq btrfs
setup_root_helper
prepare_test_dev

run_check_mkfs_test_dev
run_check_mount_test_dev

cd "$TEST_MNT"

run_check $SUDO_HELPER "$TOP/btrfs" subvolume create test1
run_check $SUDO_HELPER "$TOP/btrfs" subvolume create test1/test2
run_check $SUDO_HELPER "$TOP/btrfs" subvolume snapshot test1 test1-snap
run_check $SUDO_HELPER "$TOP/btrfs" filesystem du -s test1
run_check_stdout $SUDO_HELPER "$TOP/btrfs" filesystem du -s test1-snap | \
	grep -q "ERROR:.*ioctl" && _fail "empty subvolume not handled"

cd ..

run_check_umount_test_dev
