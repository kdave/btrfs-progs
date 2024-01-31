#!/bin/bash
# Really basic test of ntfs2btrfs conversion

source "$TEST_TOP/common" || exit
source "$TEST_TOP/common.convert" || exit

if ! check_kernel_support_ntfs >/dev/null; then
	_not_run "no NTFS support"
fi

check_prereq btrfs-convert
check_global_prereq mkfs.ntfs
check_global_prereq ntfs2btrfs

setup_root_helper
prepare_test_dev

# Iterate over defaults and options that are not tied to hardware capabilities
# or number of devices. Test only 4K block size as minimum.
for feature in '' 'block-group-tree' 'raid-stripe-tree'; do
	convert_test ntfs "$feature" "ntfs 4k nodesize" 4096 mkfs.ntfs -s 4096
done
