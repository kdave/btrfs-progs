#!/bin/bash
# Make sure btrfs-convert can handle a symbolic link which is 4095 bytes long

source "$TEST_TOP/common" || exit
source "$TEST_TOP/common.convert" || exit

setup_root_helper
prepare_test_dev 1G
check_global_prereq mkfs.ext4

# This is at the symbolic link size limit (PATH_MAX includes the terminating NUL).
link_target=$(printf "%0.sb" {1..4095})

convert_test_prep_fs ext4 mke2fs -t ext4 -b 4096
run_check $SUDO_HELPER ln -s "$link_target" "$TEST_MNT/symbolic_link"
run_check_umount_test_dev

# For unpatched btrfs-convert, it will always append one byte to the
# link target, causing the above 4095 target to be 4096, exactly one sector,
# resulting in a regular file extent.
convert_test_do_convert

run_check_mount_test_dev
# If the unpatched btrfs-convert created a regular extent, and the kernel is
# new enough, readlink will be rejected by kernel.
run_check $SUDO_HELPER readlink "$TEST_MNT/symbolic_link"
run_check_umount_test_dev
