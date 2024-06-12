#!/bin/bash
# Make sure btrfs is handling the ext4 uninit (preallocated) extent correctly

source "$TEST_TOP/common" || exit
source "$TEST_TOP/common.convert" || exit

setup_root_helper
prepare_test_dev 1G

check_global_prereq mkfs.ext4
check_global_prereq fallocate
check_global_prereq filefrag
check_global_prereq awk
check_global_prereq md5sum
check_prereq btrfs-convert
check_prereq btrfs

convert_test_prep_fs ext4 mke2fs -t ext4 -b 4096

# Create a preallocated extent first.
run_check $SUDO_HELPER fallocate -l 32K "$TEST_MNT/file"
sync
# Get the real on-disk location and write some data into it.
physical=$(run_check_stdout $SUDO_HELPER filefrag -v "$TEST_MNT/file" | grep unwritten | awk '{print $4}' | grep -o "[[:digit:]]*")

if [ -z "$physical" ]; then
	_fail "unable to get the physical address of the file"
fi

# Now fill the underlying range with non-zeros.
# For properly converted fs, we should not read the contents anyway
run_check $SUDO_HELPER dd if=/dev/urandom of="$TEST_DEV" bs=4096 seek="$physical" conv=notrunc count=8

# Write some thing into the file range.
run_check $SUDO_HELPER dd if=/dev/zero of="$TEST_MNT/file" bs=4096 count=1 conv=notrunc
run_check $SUDO_HELPER dd if=/dev/zero of="$TEST_MNT/file" bs=4096 count=1 seek=3 conv=notrunc
sync
md5_before=$(md5sum "$TEST_MNT/file" | cut -f1 -d' ')
_log "md5sum before convert: $md5_before"
run_check_umount_test_dev

# Btrfs-convert should handle the unwritten part correctly, either punching a hole
# or a proper preallocated extent, so that we won't read the on-disk data.
convert_test_do_convert

run_check_mount_test_dev
md5_after=$(md5sum "$TEST_MNT/file" | cut -f1 -d' ')
_log "md5sum after convert: $md5_after"
run_check_umount_test_dev

if [ "$md5_before" != "$md5_after" ]; then
	_fail "contents mismatch"
fi
