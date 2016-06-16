#!/bin/bash
#
# test parsing of various resize arguments

source $TOP/tests/common

check_prereq mkfs.btrfs
check_prereq btrfs

setup_root_helper
prepare_test_dev 2g

run_check $TOP/mkfs.btrfs -f $IMAGE
run_check_mount_test_dev

# missing the one of the required arguments
for sep in '' '--'; do
	run_check_stdout $TOP/btrfs filesystem resize $sep |
		grep -q "btrfs filesystem resize: too few arguments"
	run_check_stdout $TOP/btrfs filesystem resize $sep $TEST_MNT |
		grep -q "btrfs filesystem resize: too few arguments"
	run_check_stdout $TOP/btrfs filesystem resize $sep -128M |
		grep -q "btrfs filesystem resize: too few arguments"
	run_check_stdout $TOP/btrfs filesystem resize $sep +128M |
		grep -q "btrfs filesystem resize: too few arguments"
	run_check_stdout $TOP/btrfs filesystem resize $sep 512M |
		grep -q "btrfs filesystem resize: too few arguments"
	run_check_stdout $TOP/btrfs filesystem resize $sep 1:-128M |
		grep -q "btrfs filesystem resize: too few arguments"
	run_check_stdout $TOP/btrfs filesystem resize $sep 1:512M |
		grep -q "btrfs filesystem resize: too few arguments"
	run_check_stdout $TOP/btrfs filesystem resize $sep 1:+128M |
		grep -q "btrfs filesystem resize: too few arguments"
done

# valid resize
for sep in '' '--'; do
	run_check $SUDO_HELPER $TOP/btrfs filesystem resize $sep -128M $TEST_MNT
	run_check $SUDO_HELPER $TOP/btrfs filesystem resize $sep +128M $TEST_MNT
	run_check $SUDO_HELPER $TOP/btrfs filesystem resize $sep 512M $TEST_MNT
	run_check $SUDO_HELPER $TOP/btrfs filesystem resize $sep 1:-128M $TEST_MNT
	run_check $SUDO_HELPER $TOP/btrfs filesystem resize $sep 1:512M $TEST_MNT
	run_check $SUDO_HELPER $TOP/btrfs filesystem resize $sep 1:+128M $TEST_MNT
done

run_check_umount_test_dev
