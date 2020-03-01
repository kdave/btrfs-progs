#!/bin/bash
#
# Corrupt primary superblock and restore it using backup superblock.

source "$TEST_TOP/common"

check_prereq btrfs-select-super
check_prereq btrfs

setup_root_helper
prepare_test_dev

FIRST_SUPERBLOCK_OFFSET=65536

test_superblock_restore()
{
	run_check_mkfs_test_dev

	# Corrupt superblock checksum
	run_check $SUDO_HELPER dd if=/dev/zero of="$TEST_DEV" \
	seek="$FIRST_SUPERBLOCK_OFFSET" bs=1 count=4 conv=notrunc

	# Run btrfs check to detect corruption
	run_mayfail "$TOP/btrfs" check "$TEST_DEV" && \
		_fail "btrfs check should detect corruption"

	# Copy backup superblock to primary
	run_check "$INTERNAL_BIN/btrfs-select-super" -s 1 "$TEST_DEV"

	# Perform btrfs check
	run_check "$TOP/btrfs" check "$TEST_DEV"
}

test_superblock_restore
