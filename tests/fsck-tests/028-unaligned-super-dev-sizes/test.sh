#!/bin/bash
#
# An image with mis-aligned superblock total_bytes, that will be found and
# fixed by 'check' or fixed by 'rescue fix-device-size'

source "$TOP/tests/common"

check_prereq btrfs
prepare_test_dev
setup_root_helper

check_all_images

image=$(extract_image "./dev_and_super_mismatch_unaligned.raw.xz")

# detect and fix
run_check "$TOP/btrfs" rescue fix-device-size "$image"
# no problem found
run_check "$TOP/btrfs" rescue fix-device-size "$image"
# check if fix-device-size worked
run_check "$TOP/btrfs" check "$image"
# mount test
run_check_mount_test_dev
run_check_umount_test_dev

rm -f "$image"
