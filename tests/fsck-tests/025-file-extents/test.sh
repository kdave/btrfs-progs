#!/bin/bash
# Confirm btrfs check can check file extents without causing false alert

source "$TEST_TOP/common"

check_prereq btrfs
check_prereq mkfs.btrfs
check_prereq btrfstune
check_global_prereq dd
check_global_prereq fallocate
check_global_prereq truncate

setup_root_helper
prepare_test_dev 128M

# Do some write into a large prealloc range
# Lowmem mode can report missing csum due to wrong csum range
test_partial_write_into_prealloc()
{
	run_check_mkfs_test_dev
	run_check_mount_test_dev

	run_check $SUDO_HELPER fallocate -l 128K "$TEST_MNT/file"
	sync
	run_check $SUDO_HELPER dd conv=notrunc if=/dev/zero of="$TEST_MNT/file" bs=1K count=64
	run_check_umount_test_dev
	run_check "$TOP/btrfs" check "$TEST_DEV"
}

# Inline compressed file extent
# Lowmem mode can cause silent error without any error message
# due to too restrict check on inline extent size
test_compressed_inline_extent()
{
	run_check_mkfs_test_dev
	run_check_mount_test_dev -o compress=lzo,max_inline=2048

	run_check $SUDO_HELPER dd conv=notrunc if=/dev/null of="$TEST_MNT/file" bs=1K count=1
	run_check_umount_test_dev
	run_check "$TOP/btrfs" check "$TEST_DEV"
}

# File extent hole with NO_HOLES incompat feature set.
# Lowmem mode will cause a false alert as it doesn't allow any file hole
# extents, while we can set NO_HOLES at anytime we want, it's definitely a
# false alert
test_hole_extent_with_no_holes_flag()
{
	run_check_mkfs_test_dev
	run_check_mount_test_dev

	run_check $SUDO_HELPER truncate -s 16K "$TEST_MNT/tmp"
	run_check_umount_test_dev
	run_check $SUDO_HELPER "$TOP/btrfstune" -n "$TEST_DEV"
	run_check "$TOP/btrfs" check "$TEST_DEV"
}

test_partial_write_into_prealloc
test_compressed_inline_extent
test_hole_extent_with_no_holes_flag
