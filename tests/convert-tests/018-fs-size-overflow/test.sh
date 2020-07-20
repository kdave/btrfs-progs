#!/bin/bash
# Check if btrfs-convert can handle an ext4 fs whose size is 64G.
# That fs size could trigger a multiply overflow and screw up free space
# calculation

source "$TEST_TOP/common"
source "$TEST_TOP/common.convert"

setup_root_helper
prepare_test_dev 64g
check_prereq btrfs-convert
check_global_prereq mke2fs

convert_test_prep_fs ext4 mke2fs -t ext4 -b 4096
run_check_umount_test_dev

# Unpatched btrfs-convert would fail half way due to corrupted free space
# cache tree
convert_test_do_convert
