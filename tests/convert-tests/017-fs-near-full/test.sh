#!/bin/bash
# Check if btrfs-convert creates filesystem with device extents beyond the
# device boundary

source "$TEST_TOP/common"
source "$TEST_TOP/common.convert"

setup_root_helper
prepare_test_dev 1G
check_prereq btrfs-convert
check_global_prereq mke2fs
check_global_prereq fallocate

convert_test_prep_fs ext4 mke2fs -t ext4 -b 4096

# Use up 800MiB first
for i in $(seq 1 4); do
	run_check $SUDO_HELPER fallocate -l 200M "$TEST_MNT/file$i"
done

# Then add 5MiB for above files. These 5 MiB will be allocated near the very
# end of the fs, to confuse btrfs-convert
for i in $(seq 1 4); do
	run_check $SUDO_HELPER fallocate -l 205M "$TEST_MNT/file$i"
done

run_check_umount_test_dev

# convert_test_do_convert() will call btrfs check, which should expose any
# invalid inline extent with too large size
convert_test_do_convert
