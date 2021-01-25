#!/bin/bash
#
# test parsing of various resize arguments

source "$TEST_TOP/common"

check_prereq mkfs.btrfs
check_prereq btrfs

setup_root_helper
prepare_test_dev 2g

run_check_mkfs_test_dev
run_check_mount_test_dev

# missing the one of the required arguments
for sep in '' '--'; do
	run_check_stdout "$TOP/btrfs" filesystem resize $sep |
		grep -q "btrfs filesystem resize: exactly 2 arguments expected, 0 given" ||
		_fail "no expected error message in the output"
	run_check_stdout "$TOP/btrfs" filesystem resize $sep "$TEST_MNT" |
		grep -q "btrfs filesystem resize: exactly 2 arguments expected, 1 given" ||
		_fail "no expected error message in the output"
	run_check_stdout "$TOP/btrfs" filesystem resize $sep -128M |
		grep -q "btrfs filesystem resize: exactly 2 arguments expected, 1 given" ||
		_fail "no expected error message in the output"
	run_check_stdout "$TOP/btrfs" filesystem resize $sep +128M |
		grep -q "btrfs filesystem resize: exactly 2 arguments expected, 1 given" ||
		_fail "no expected error message in the output"
	run_check_stdout "$TOP/btrfs" filesystem resize $sep 512M |
		grep -q "btrfs filesystem resize: exactly 2 arguments expected, 1 given" ||
		_fail "no expected error message in the output"
	run_check_stdout "$TOP/btrfs" filesystem resize $sep 1:-128M |
		grep -q "btrfs filesystem resize: exactly 2 arguments expected, 1 given" ||
		_fail "no expected error message in the output"
	run_check_stdout "$TOP/btrfs" filesystem resize $sep 1:512M |
		grep -q "btrfs filesystem resize: exactly 2 arguments expected, 1 given" ||
		_fail "no expected error message in the output"
	run_check_stdout "$TOP/btrfs" filesystem resize $sep 1:+128M |
		grep -q "btrfs filesystem resize: exactly 2 arguments expected, 1 given" ||
		_fail "no expected error message in the output"
done

# valid resize
for sep in '' '--'; do
	run_check $SUDO_HELPER "$TOP/btrfs" filesystem resize $sep -128M "$TEST_MNT"
	run_check $SUDO_HELPER "$TOP/btrfs" filesystem resize $sep +128M "$TEST_MNT"
	run_check $SUDO_HELPER "$TOP/btrfs" filesystem resize $sep 512M "$TEST_MNT"
	run_check $SUDO_HELPER "$TOP/btrfs" filesystem resize $sep 1:-128M "$TEST_MNT"
	run_check $SUDO_HELPER "$TOP/btrfs" filesystem resize $sep 1:512M "$TEST_MNT"
	run_check $SUDO_HELPER "$TOP/btrfs" filesystem resize $sep 1:+128M "$TEST_MNT"
	run_check $SUDO_HELPER "$TOP/btrfs" filesystem resize $sep max "$TEST_MNT"
	run_check $SUDO_HELPER "$TOP/btrfs" filesystem resize $sep -128M "$TEST_MNT"
	run_check $SUDO_HELPER "$TOP/btrfs" filesystem resize $sep 1:max "$TEST_MNT"
done

# Test passing a file instead of a directory
run_mustfail_stdout "should fail for image" \
	"$TOP/btrfs" filesystem resize 1:-128M "$TEST_DEV" |
	_log_stdout |
	grep -q "ERROR: resize works on mounted filesystems and accepts only" ||
	_fail "no expected error message in the output 2"

run_check_umount_test_dev
