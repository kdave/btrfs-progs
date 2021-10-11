#!/bin/bash
# Test if mkfs.btrfs --rootdir will skip shrinking correctly

source "$TEST_TOP/common"

check_prereq mkfs.btrfs

setup_root_helper

fs_size=$((512 * 1024 * 1024))
bs=$((1024 * 1024))
tmp=$(_mktemp_dir mkfs-rootdir)

prepare_test_dev "$fs_size"

# No shrink case

run_check_mkfs_test_dev --rootdir "$tmp"
run_check_mount_test_dev

# We should be able to write at least half of the fs size data since the fs is
# not shrunk
run_check $SUDO_HELPER dd if=/dev/zero bs="$bs" count=$(($fs_size / $bs / 2)) \
	of="$TEST_MNT/file"

run_check_umount_test_dev

# Shrink case

run_check_mkfs_test_dev --rootdir "$tmp" --shrink
run_check_mount_test_dev

run_mustfail "mkfs.btrfs for shrink rootdir" \
	$SUDO_HELPER dd if=/dev/zero bs="$bs" count=$(($fs_size / $bs / 2)) \
	of="$TEST_MNT/file"

run_check_umount_test_dev

rm -rf -- "$tmp"
