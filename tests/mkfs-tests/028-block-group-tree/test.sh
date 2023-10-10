#!/bin/bash
# Basic check if mkfs supports the block-group-tree

source "$TEST_TOP/common" || exit

check_prereq mkfs.btrfs
check_prereq btrfs

setup_root_helper
prepare_test_dev

run_check_mkfs_test_dev -O block-group-tree

run_check_stdout $SUDO_HELPER "$TOP/btrfs" inspect-internal dump-super "$TEST_DEV" | \
	grep -q BLOCK_GROUP_TREE || _fail "block group tree not created"
