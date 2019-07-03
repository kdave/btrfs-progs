#!/bin/bash

# Specially created e2image dump to test backup superblock migration for
# new convert.
# These images will cause the following problems if convert doesn't handle
# backup superblock migration well:
# 1) Assert while building free space tree
# 2) Error copying inodes
# 3) Discontinuous file extents after convert
# 4) Overlap file extents
# 5) Unable to rollback

source "$TEST_TOP/common"

check_prereq btrfs-convert
check_prereq btrfs
check_global_prereq e2fsck
check_global_prereq xzcat

setup_root_helper
prepare_test_dev

# override common function
check_image() {
	TEST_DEV="$1"
	run_check e2fsck -n -f "$TEST_DEV"
	run_check "$TOP/btrfs-convert" "$TEST_DEV"
	run_check "$TOP/btrfs" check "$TEST_DEV"
	run_check "$TOP/btrfs" inspect-internal dump-super "$TEST_DEV"

	run_check_mount_test_dev
	run_check $SUDO_HELPER e2fsck -n -f "$TEST_MNT/ext2_saved/image"
	run_check $SUDO_HELPER umount "$TEST_MNT"

	run_check "$TOP/btrfs" check "$TEST_DEV"
	run_check "$TOP/btrfs-convert" -r "$TEST_DEV"
	run_check e2fsck -n -f "$TEST_DEV"

	rm -f "$TEST_DEV"
}

check_all_images
