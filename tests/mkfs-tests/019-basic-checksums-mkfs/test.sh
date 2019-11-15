#!/bin/bash
#
# Test creating images with all supported checksums, mount checks are separate

source "$TEST_TOP/common"

check_prereq mkfs.btrfs
check_prereq btrfs

setup_root_helper
prepare_test_dev

test_mkfs_checksum()
{
	local csum

	csum="$1"
	run_check_stdout $SUDO_HELPER "$TOP/mkfs.btrfs" -f --csum "$csum" "$TEST_DEV" | grep -q "Checksum:.*$csum"
	run_check $SUDO_HELPER "$TOP/btrfs" inspect-internal dump-super "$TEST_DEV"
	run_check $SUDO_HELPER "$TOP/btrfs" check "$TEST_DEV"
}

test_mkfs_checksum crc32c
test_mkfs_checksum xxhash
test_mkfs_checksum sha256
test_mkfs_checksum blake2
