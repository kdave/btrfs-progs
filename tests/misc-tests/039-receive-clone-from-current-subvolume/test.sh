#!/bin/bash
# Test that when receiving a subvolume whose received UUID already exists in
# the filesystem, we clone from the correct source (the subvolume that we are
# receiving, not the existing subvolume). This is a regression test for
# "btrfs-progs: receive: don't lookup clone root for received subvolume".

source "$TEST_TOP/common"

check_prereq btrfs
check_prereq mkfs.btrfs

setup_root_helper

rm -f disk
run_check truncate -s 1G disk
chmod a+w disk
run_check $SUDO_HELPER "$TOP/mkfs.btrfs" -f disk
run_check $SUDO_HELPER mount -o loop disk "$TEST_MNT"

run_check $SUDO_HELPER "$TOP/btrfs" subvolume create "$TEST_MNT/subvol"
run_check $SUDO_HELPER dd if=/dev/urandom of="$TEST_MNT/subvol/foo" \
	bs=1M count=1 status=none
run_check $SUDO_HELPER cp --reflink "$TEST_MNT/subvol/foo" "$TEST_MNT/subvol/bar"
run_check $SUDO_HELPER mkdir "$TEST_MNT/subvol/dir"
run_check $SUDO_HELPER mv "$TEST_MNT/subvol/foo" "$TEST_MNT/subvol/dir"
run_check $SUDO_HELPER "$TOP/btrfs" property set "$TEST_MNT/subvol" ro true
run_check $SUDO_HELPER "$TOP/btrfs" send -f send.data "$TEST_MNT/subvol"

run_check $SUDO_HELPER mkdir "$TEST_MNT/first" "$TEST_MNT/second"
run_check $SUDO_HELPER "$TOP/btrfs" receive -f send.data "$TEST_MNT/first"
run_check $SUDO_HELPER "$TOP/btrfs" receive -f send.data "$TEST_MNT/second"

run_check $SUDO_HELPER umount "$TEST_MNT"
rm -f disk send.data
