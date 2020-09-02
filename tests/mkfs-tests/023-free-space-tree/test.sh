#!/bin/bash
# Basic check if mkfs supports the runtime feature free-space-tree

source "$TEST_TOP/common"

check_prereq mkfs.btrfs
check_prereq btrfs

setup_root_helper
prepare_test_dev

run_check_mkfs_test_dev -R free-space-tree

run_check_stdout $SUDO_HELPER "$TOP/btrfs" inspect-internal dump-super "$TEST_DEV" | \
	grep -q FREE_SPACE_TREE || _fail "free space tree not created"
run_check_stdout $SUDO_HELPER "$TOP/btrfs" inspect-internal dump-super "$TEST_DEV" | \
	grep -q FREE_SPACE_TREE_VALID || _fail "free space tree not valid"
