#!/bin/bash
# Check that list-chunks reports usage with block-group-tree.

source "$TEST_TOP/common" || exit

setup_root_helper
prepare_test_dev

check_prereq btrfs
check_prereq mkfs.btrfs

if [ ! -f /sys/fs/btrfs/features/block_group_tree ]; then
	_not_run "block-group-tree feature not supported"
fi

run_check_mkfs_test_dev -O block-group-tree
run_check_mount_test_dev

run_check $SUDO_HELPER dd if=/dev/zero of="$TEST_MNT/file" bs=1M count=100 status=none
run_check "$TOP/btrfs" filesystem sync "$TEST_MNT"

output=$(run_check_stdout $SUDO_HELPER "$TOP/btrfs" inspect-internal list-chunks "$TEST_MNT")

if echo "$output" | grep -q "not found"; then
	_fail "list-chunks cannot find block group items"
fi

if ! echo "$output" | sed '1,2d; s/ *$//' | grep -qv ' 0\.00$'; then
	_fail "list-chunks reports zero usage for all chunks"
fi

run_check_umount_test_dev
