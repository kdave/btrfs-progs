#!/bin/bash
# Verify that receive --dump escapes paths for rename, symlink and hardlink
# when it's the "dest=" value

source "$TEST_TOP/common"

setup_root_helper
prepare_test_dev

run_check_mkfs_test_dev
run_check_mount_test_dev

# Create filenames so if the newlines are not properly escaped the word
# 'failed' appears on the first line in the dump

# Symlink
run_check $SUDO_HELPER "$TOP/btrfs" subvolume create "$TEST_MNT/subv1"
run_check $SUDO_HELPER ln -s "$TEST_MNT/subv1/file
failed symlink source" "$TEST_MNT/subv1/file
failed symlink target"

# Hardlinke
run_check $SUDO_HELPER touch "$TEST_MNT/subv1/file
failed link source"

run_check $SUDO_HELPER ln "$TEST_MNT/subv1/file
failed link source" "$TEST_MNT/subv1/file
failed link target"

run_check $SUDO_HELPER "$TOP/btrfs" subvolume snapshot -r "$TEST_MNT/subv1" "$TEST_MNT/snap1"
_mktemp_local send.stream
run_check $SUDO_HELPER "$TOP/btrfs" send -f send.stream "$TEST_MNT/snap1"

run_check_stdout "$TOP/btrfs" receive --dump -f send.stream |
	grep '^failed' && _fail "newlines not escaped in stream dump"

run_check_umount_test_dev
rm -- send.stream
