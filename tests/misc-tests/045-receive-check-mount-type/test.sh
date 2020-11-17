#!/bin/bash
#
# Test some scenarios around the mount point we do receive onto.
# Should fail in a non-btrfs filesystem, but succeed if a non btrfs filesystem
# is the longest mounted substring of the target, but not the actual containing
# mount.
#
# This is a regression test for
# "btrfs-progs: receive: fix btrfs_mount_root substring bug"

source "$TEST_TOP/common"

check_prereq mkfs.btrfs
check_prereq btrfs

setup_root_helper
prepare_test_dev

run_check_mkfs_test_dev
run_check_mount_test_dev

cd "$TEST_MNT"
run_check $SUDO_HELPER mkdir "foo" "foobar"
run_check $SUDO_HELPER mount -t tmpfs tmpfs "foo"
run_check $SUDO_HELPER mkdir "foo/bar"

run_check $SUDO_HELPER "$TOP/btrfs" subvolume create "subvol"
run_check $SUDO_HELPER "$TOP/btrfs" subvolume snapshot -r "subvol" "snap"
run_check $SUDO_HELPER "$TOP/btrfs" send -f send.data "snap"
run_mustfail "no receive on tmpfs" $SUDO_HELPER "$TOP/btrfs" receive -f send.data "./foo"
run_mustfail "no receive on tmpfs" $SUDO_HELPER "$TOP/btrfs" receive -f send.data "./foo/bar"
run_check $SUDO_HELPER "$TOP/btrfs" receive -f send.data "./foobar"
run_check_umount_test_dev "foo"

cd ..
run_check_umount_test_dev
