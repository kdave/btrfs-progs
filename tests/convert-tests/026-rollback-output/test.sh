#!/bin/bash
# Make sure "btrfs-convert -r" is outputting the correct filename

source "$TEST_TOP/common" || exit
source "$TEST_TOP/common.convert" || exit

setup_root_helper
prepare_test_dev

check_global_prereq mkfs.ext4
check_prereq btrfs-convert
check_prereq btrfs

convert_test_prep_fs ext4 mke2fs -t ext4 -b 4096
run_check_umount_test_dev
convert_test_do_convert

# Rollback and save the output.
run_check_stdout "$TOP/btrfs-convert" --rollback "$TEST_DEV" |\
	grep -q "ext2_saved/image" || _fail "rollback filename output is corruptedd"
