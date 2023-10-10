#!/bin/bash
# Basic check if mkfs supports the raid-stripe-tree

source "$TEST_TOP/common" || exit

check_prereq mkfs.btrfs
check_prereq btrfs

setup_root_helper
prepare_test_dev

run_check_mkfs_test_dev -O raid-stripe-tree

run_check_stdout $SUDO_HELPER "$TOP/btrfs" inspect-internal dump-super "$TEST_DEV" | \
	grep -q RAID_STRIPE_TREE || _fail "raid stripe tree not created"
