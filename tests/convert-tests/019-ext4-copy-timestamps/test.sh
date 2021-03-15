#!/bin/bash
# Check if [acm]time values are copied from ext4 with full precision

source "$TEST_TOP/common"
source "$TEST_TOP/common.convert"

setup_root_helper
prepare_test_dev
check_prereq btrfs-convert
check_global_prereq mke2fs

convert_test_prep_fs ext4 mke2fs -t ext4 -b 4096

features=$(run_check_stdout dumpe2fs "$TEST_DEV" | grep "Filesystem features")
if ! echo "$features" | grep -q "extra_isize"; then
	_not_run "extended inode size not supported, no 64bit timestamp check possible"
fi

run_check $SUDO_HELPER touch "$TEST_MNT/file"
# Read time values
run_check stat "$TEST_MNT/file"
atime=$(run_check_stdout stat --format=%x "$TEST_MNT/file")
mtime=$(run_check_stdout stat --format=%y "$TEST_MNT/file")
ctime=$(run_check_stdout stat --format=%z "$TEST_MNT/file")
run_check_umount_test_dev

convert_test_do_convert

run_check_mount_test_dev
run_check stat "$TEST_MNT/file"
# Verify
time=$(run_check_stdout stat --format=%x "$TEST_MNT/file")
if [ "$time" != "$atime" ]; then
	_fail "atime on converted inode does not match"
fi
time=$(run_check_stdout stat --format=%y "$TEST_MNT/file")
if [ "$time" != "$mtime" ]; then
	_fail "mtime on converted inoded does not match"
fi
time=$(run_check_stdout stat --format=%z "$TEST_MNT/file")
if [ "$time" != "$mtime" ]; then
	_fail "ctime on converted inoded does not match"
fi
run_check_umount_test_dev
