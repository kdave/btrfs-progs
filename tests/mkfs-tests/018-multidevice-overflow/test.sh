#!/bin/bash
# test if mkfs.btrfs will create file systems that overflow total_bytes

source "$TEST_TOP/common"

check_prereq mkfs.btrfs
check_prereq btrfs

setup_root_helper
prepare_test_dev

# create a temporary btrfs filesystem for the images to make sure the
# exabyte-scale files will be reliably created
run_check_mkfs_test_dev
run_check_mount_test_dev

# truncate can fail with EFBIG if the OS cannot create a 6EiB file
run_mayfail $SUDO_HELPER truncate -s 6E "$TEST_MNT/img1"
ret=$?
if [ $ret == 27 ]; then
	_not_run "Current kernel could not create a 6E file"
fi

if [ $ret -gt 0 ]; then
	_fail "truncate -s 6E failed: $ret"
fi

run_check $SUDO_HELPER truncate -s 6E "$TEST_MNT/img2"
run_check $SUDO_HELPER truncate -s 6E "$TEST_MNT/img3"

run_mustfail "mkfs for too-large images" \
	$SUDO_HELPER "$TOP/mkfs.btrfs" -f "$TEST_MNT"/img[123]

run_check_umount_test_dev
