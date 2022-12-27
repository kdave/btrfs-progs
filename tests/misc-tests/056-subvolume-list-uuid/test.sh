#!/bin/bash
#
# Make sure "btrfs subvolume list -u" shows uuid correctly

source "$TEST_TOP/common"

check_prereq mkfs.btrfs
check_prereq btrfs
check_global_prereq uuidparse

setup_root_helper
prepare_test_dev

tmp=$(_mktemp_dir list_uuid)

run_check_mkfs_test_dev
run_check_mount_test_dev
run_check $SUDO_HELPER "$TOP/btrfs" subvolume create "$TEST_MNT/subv1"
run_check_stdout $SUDO_HELPER "$TOP/btrfs" subvolume list -u "$TEST_MNT" | \
	cut -d\  -f9 > "$tmp/output"

result=$(cat "$tmp/output" | uuidparse -o TYPE -n)
rm -rf -- "$tmp"

if [ "$result" == "invalid" ]; then
	_fail "subvolume list failed to report uuid"
fi
run_check_umount_test_dev
