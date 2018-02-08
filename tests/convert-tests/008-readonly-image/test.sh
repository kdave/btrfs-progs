#!/bin/bash
# Check if the converted ext2 image is readonly

source "$TEST_TOP/common"
source "$TEST_TOP/common.convert"

setup_root_helper
prepare_test_dev
check_prereq btrfs-convert
check_global_prereq mke2fs

default_mke2fs="mke2fs -t ext4 -b 4096"
convert_test_preamble '' 'readonly image test' 16k "$default_mke2fs"
convert_test_prep_fs ext4 $default_mke2fs
run_check_umount_test_dev
convert_test_do_convert
run_check_mount_test_dev

# It's expected to fail
$SUDO_HELPER dd if=/dev/zero of="$TEST_MNT/ext2_saved/image" bs=1M count=1 \
	&> /dev/null
if [ $? -ne 1 ]; then
	echo "after convert ext2_saved/image is not read-only"
	exit 1
fi
run_check_umount_test_dev
convert_test_post_rollback ext4
