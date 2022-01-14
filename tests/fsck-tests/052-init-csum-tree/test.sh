#!/bin/bash
#
# Verify that `btrfs check --init-csum-tree` can handle various nodatasum
# cases.

source "$TEST_TOP/common"

check_prereq btrfs
check_global_prereq fallocate
check_global_prereq dd

setup_root_helper
prepare_test_dev

run_check_mkfs_test_dev

# Create an inode with nodatasum and some content
run_check_mount_test_dev -o nodatasum

run_check $SUDO_HELPER dd if=/dev/urandom of="$TEST_MNT/nodatasum_file" \
	bs=16k count=1 status=noxfer > /dev/null 2>&1

# Revert to default datasum
run_check $SUDO_HELPER mount -o remount,datasum "$TEST_MNT"

# Then create an inode with datasum, but all preallocated extents
run_check $SUDO_HELPER fallocate -l 32k "$TEST_MNT/prealloc1"

# Create preallocated extent but partially written
run_check $SUDO_HELPER fallocate -l 32k "$TEST_MNT/prealloc2"
run_check $SUDO_HELPER dd if=/dev/urandom of="$TEST_MNT/prealloc2" \
	bs=16k count=1 conv=notrunc status=noxfer> /dev/null 2>&1

# Then some regular files
run_check $SUDO_HELPER dd if=/dev/urandom of="$TEST_MNT/regular" \
	bs=16k count=1 status=noxfer > /dev/null 2>&1

# And some regular files with holes
run_check $SUDO_HELPER dd if=/dev/urandom of="$TEST_MNT/regular_with_holes" \
	bs=16k count=1 status=noxfer > /dev/null 2>&1
run_check $SUDO_HELPER fallocate -p -o 4096 -l 4096 "$TEST_MNT/regular_with_holes"

# And the most complex one, preallocated, written, then hole
run_check $SUDO_HELPER fallocate  -l 8192 "$TEST_MNT/complex"
run_check $SUDO_HELPER dd if=/dev/urandom of="$TEST_MNT/compex" \
	bs=4k count=1 conv=notrunc status=noxfer > /dev/null 2>&1
sync
run_check $SUDO_HELPER fallocate -p -l 4096 "$TEST_MNT/regular_with_holes"

# Create some compressed file
run_check $SUDO_HELPER mount -o remount,compress "$TEST_MNT"
run_check $SUDO_HELPER dd if=/dev/zero of="$TEST_MNT/compressed" \
	bs=16k count=4 conv=notrunc status=noxfer> /dev/null 2>&1

# And create a snapshot, every data extent is at least shared twice
run_check $SUDO_HELPER "$TOP/btrfs" subvolume snapshot "$TEST_MNT" \
	"$TEST_MNT/snapshot"
run_check_umount_test_dev

# --init-csum-tree should not fail
run_check $SUDO_HELPER "$TOP/btrfs" check --force \
	--init-csum-tree "$TEST_DEV"

# No error should be found
run_check $SUDO_HELPER "$TOP/btrfs" check "$TEST_DEV"
