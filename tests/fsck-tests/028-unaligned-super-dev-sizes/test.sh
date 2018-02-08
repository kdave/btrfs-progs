#!/bin/bash
#
# An image with mis-aligned superblock total_bytes, that will be found and
# fixed by 'check' or fixed by 'rescue fix-device-size'

source "$TEST_TOP/common"

check_prereq btrfs
setup_root_helper

check_all_images

TEST_DEV=$(extract_image "./dev_and_super_mismatch_unaligned.raw.xz")

# detect and fix
run_check "$TOP/btrfs" rescue fix-device-size "$TEST_DEV"
# no problem found
run_check "$TOP/btrfs" rescue fix-device-size "$TEST_DEV"
# check if fix-device-size worked
run_check "$TOP/btrfs" check "$TEST_DEV"
# mount test
run_check_mount_test_dev
run_check_umount_test_dev "$TEST_MNT"
# remove restored image
rm -- "$TEST_DEV"
