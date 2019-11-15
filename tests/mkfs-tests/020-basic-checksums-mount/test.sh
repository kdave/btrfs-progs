#!/bin/bash
#
# Test creating images with all supported checksums followed by mount

source "$TEST_TOP/common"

check_prereq mkfs.btrfs
check_prereq btrfs

setup_root_helper
prepare_test_dev

test_mkfs_mount_checksum()
{
	run_check_stdout $SUDO_HELPER "$TOP/mkfs.btrfs" -f --csum "$csum" "$TEST_DEV" | grep -q "Checksum:.*$csum"
	run_check $SUDO_HELPER "$TOP/btrfs" inspect-internal dump-super "$TEST_DEV"
	run_check $SUDO_HELPER "$TOP/btrfs" check "$TEST_DEV"

	run_check $SUDO_HELPER mount "$dev1" "$TEST_MNT"
	run_check "$TOP/btrfs" filesystem df "$TEST_MNT"
	run_check $SUDO_HELPER "$TOP/btrfs" filesystem usage "$TEST_MNT"
	run_check $SUDO_HELPER "$TOP/btrfs" device usage "$TEST_MNT"
	run_check $SUDO_HELPER umount "$TEST_MNT"
}

if ! [ -f "/sys/fs/btrfs/features/supported_checksums" ]; then
	_not_run "kernel support for checksums missing"
fi

test_mkfs_mount_checksum crc32c
test_mkfs_mount_checksum xxhash
test_mkfs_mount_checksum sha256
test_mkfs_mount_checksum blake2
