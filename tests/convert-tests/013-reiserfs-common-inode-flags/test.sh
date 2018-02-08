#!/bin/bash
# Check if btrfs-convert can copy common inode flags like SYNC/IMMUTABLE

source "$TEST_TOP/common"
source "$TEST_TOP/common.convert"

if ! check_kernel_support_reiserfs >/dev/null; then
	_not_run "no reiserfs support"
fi

setup_root_helper
prepare_test_dev
check_prereq btrfs-convert
check_global_prereq mkreiserfs
check_global_prereq chattr
check_global_prereq lsattr

fail=0
default_mkfs="mkreiserfs -b 4096"
convert_test_preamble '' 'common inode flags test' 16k "$default_mkfs"
convert_test_prep_fs reiserfs $default_mkfs

# create file with specific flags
run_check $SUDO_HELPER touch "$TEST_MNT/flag_test"
run_check $SUDO_HELPER chattr +aAdSi "$TEST_MNT/flag_test"
run_check lsattr "$TEST_MNT/flag_test"

run_check_umount_test_dev
convert_test_do_convert
run_check_mount_test_dev

# Log the status
run_check lsattr "$TEST_MNT/flag_test"
# Above flags should be copied to btrfs flags, and lsattr should get them
run_check_stdout lsattr "$TEST_MNT/flag_test" | cut -f1 -d\ | grep "[aAdiS]" -q
if [ $? -ne 0 ]; then
	rm tmp_output
	_fail "no common inode flags are copied after convert"
fi

run_check_umount_test_dev
convert_test_post_rollback reiserfs
