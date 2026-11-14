#!/bin/bash
# Check if btrfs-convert creates filesystem with device extents beyond the
# device boundary

source "$TEST_TOP/common" || exit
source "$TEST_TOP/common.convert" || exit

check_prereq btrfs-convert
check_global_prereq mke2fs
check_global_prereq fallocate

setup_root_helper
prepare_test_dev 1G

convert_test_prep_fs ext4 mke2fs -t ext4 -b 4096

# Use up 800MiB first
for i in {1..4}; do
	run_check $SUDO_HELPER fallocate -l 200M "$TEST_MNT/file$i"
done

# Then add 5MiB for above files. These 5 MiB will be allocated near the very
# end of the fs, to confuse btrfs-convert
for i in {1..4}; do
	run_check $SUDO_HELPER fallocate -l 205M "$TEST_MNT/file$i"
done

run_check_umount_test_dev

export BTRFS_PROGS_DEBUG_STRICT_CHUNK_ALIGNMENT=y
# The e2mkfs may use extra space due to their default feature changes.
# So the convert itself may fail due to lack of space.
run_mayfail "$TOP/btrfs-convert" "$TEST_DEV"
if [ $? -ne 0 ]; then
	_not_run "Not enough space left for btrfs-convert"
fi
run_check "$TOP/btrfs" check "$TEST_DEV"
run_check "$TOP/btrfs" inspect-internal dump-super -Ffa "$TEST_DEV"
