#!/bin/bash
# Test btrfstune --convert-to-free-space-tree option

source "$TEST_TOP/common" || exit
source "$TEST_TOP/common.convert" || exit

check_prereq mkfs.btrfs
check_prereq btrfstune
check_prereq btrfs

setup_root_helper
prepare_test_dev
check_kernel_support_acl

run_check_mkfs_test_dev -O ^free-space-tree
run_check_mount_test_dev
populate_fs
run_check_umount_test_dev

# Check if the fs has free space tree already. Currently bs < ps mount
# will always enable free-space-tree (no support for v1 free space cache)
if run_check_stdout "$TOP/btrfs" inspect-internal dump-super "$TEST_DEV" |\
	grep -q "FREE_SPACE_TREE"; then
	_not_run "free-space-tree is always enabled for page size $(getconf PAGESIZE)"
fi

run_check $SUDO_HELPER "$TOP/btrfstune" --convert-to-free-space-tree "$TEST_DEV"


run_check "$TOP/btrfs" check "$TEST_DEV"
