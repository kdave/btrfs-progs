#!/bin/bash
# Verify mkfs for zoned devices support block-group-tree feature

source "$TEST_TOP/common" || exit

setup_root_helper
# Create one 128M device with 4M zones, 32 of them
setup_nullbdevs 1 128 4

prepare_nullbdevs

TEST_DEV="${nullb_devs[1]}"
# Use single as it's supported on more kernels
run_check $SUDO_HELPER "$TOP/mkfs.btrfs" -m single -d single -O block-group-tree "$TEST_DEV"
run_check_mount_test_dev
run_check $SUDO_HELPER dd if=/dev/zero of="$TEST_MNT"/file bs=1M count=1
run_check $SUDO_HELPER "$TOP/btrfs" filesystem usage -T "$TEST_MNT"
run_check_umount_test_dev

cleanup_nullbdevs
