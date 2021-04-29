#!/bin/bash
# Verify that the restored image of an empty btrfs filesystem can still be
# mounted

source "$TEST_TOP/common"

check_prereq btrfs-image
check_prereq mkfs.btrfs
check_prereq btrfs

prepare_test_dev

tmp=$(mktemp -d --tmpdir btrfs-progs-image.XXXXXXXX)

run_check_mkfs_test_dev
run_check "$TOP/btrfs-image" "$TEST_DEV" "$tmp/dump"
run_check "$TOP/btrfs-image" -r "$tmp/dump" "$tmp/restored"
run_check $SUDO_HELPER mount -t btrfs -o loop "$tmp/restored" "$TEST_MNT"
run_check $SUDO_HELPER umount "$TEST_MNT"

rm -rf -- "$tmp"
