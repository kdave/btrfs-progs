#!/bin/bash
# Check if the converted ext2 image is readonly

source $TOP/tests/common
source $TOP/tests/common.convert

setup_root_helper
prepare_test_dev 512M
check_prereq btrfs-convert

default_mke2fs="mke2fs -t ext4 -b 4096"
convert_test_preamble '' 'readonly image test' 16k "$default_mke2fs"
convert_test_prep_fs $default_mke2fs
run_check_umount_test_dev
convert_test_do_convert
run_check_mount_test_dev

# It's expected to fail
$SUDO_HELPER dd if=/dev/zero of=$TEST_MNT/ext2_save/image bs=1M count=1 \
	&> /dev/null
if [ $? -ne 1 ]; then
	echo "after convert ext2_save/image is not read-only"
	exit 1
fi
run_check_umount_test_dev
convert_test_post_rollback
