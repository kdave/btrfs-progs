#!/bin/bash
# Make sure btrfs-convert can create a fs with bgt feature.

source "$TEST_TOP/common" || exit
source "$TEST_TOP/common.convert" || exit

setup_root_helper
prepare_test_dev

check_global_prereq mkfs.ext4
check_prereq btrfs-convert
check_prereq btrfs

convert_test_prep_fs ext4 mke2fs -t ext4 -b 4096
run_check_umount_test_dev
convert_test_do_convert bgt 16384

# Manually check the super block to make sure it has BGT flag.
run_check_stdout "$TOP/btrfs" inspect-internal dump-super "$TEST_DEV" |\
	grep -q "BLOCK_GROUP_TREE" || _fail "No block-group-tree feature enabled"
