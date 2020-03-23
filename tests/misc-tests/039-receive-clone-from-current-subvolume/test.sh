#!/bin/bash
# Test that when receiving a subvolume whose received UUID already exists in
# the filesystem, we clone from the correct source (the subvolume that we are
# receiving, not the existing subvolume). This is a regression test for
# "btrfs-progs: receive: don't lookup clone root for received subvolume".

source "$TEST_TOP/common"

check_prereq btrfs
check_prereq mkfs.btrfs

setup_root_helper
prepare_test_dev

run_check_mkfs_test_dev
run_check_mount_test_dev

cd "$TEST_MNT"

run_check $SUDO_HELPER "$TOP/btrfs" subvolume create "subvol"
run_check $SUDO_HELPER dd if=/dev/urandom of="subvol/foo" bs=1M count=1 status=none
run_check $SUDO_HELPER cp --reflink "subvol/foo" "subvol/bar"
run_check $SUDO_HELPER mkdir "subvol/dir"
run_check $SUDO_HELPER mv "subvol/foo" "subvol/dir"
run_check $SUDO_HELPER "$TOP/btrfs" property set "subvol" ro true
run_check $SUDO_HELPER "$TOP/btrfs" send -f send.data "subvol"
run_check $SUDO_HELPER mkdir "first" "second"
run_check $SUDO_HELPER "$TOP/btrfs" receive -f send.data "first"
run_check $SUDO_HELPER "$TOP/btrfs" receive -f send.data "second"

cd ..
run_check_umount_test_dev
