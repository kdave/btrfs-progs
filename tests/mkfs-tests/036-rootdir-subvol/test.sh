#!/bin/bash
# Basic test for mkfs.btrfs --subvol option

source "$TEST_TOP/common" || exit

check_prereq mkfs.btrfs
check_prereq btrfs

setup_root_helper
prepare_test_dev

tmp=$(_mktemp_dir mkfs-rootdir)

run_check touch "$tmp/foo"
run_check mkdir "$tmp/dir"
run_check mkdir "$tmp/dir/subvol"
run_check touch "$tmp/dir/subvol/bar"

run_check_mkfs_test_dev --rootdir "$tmp" --subvol dir/subvol
run_check $SUDO_HELPER "$TOP/btrfs" check "$TEST_DEV"

run_check_mount_test_dev
run_check_stdout $SUDO_HELPER "$TOP/btrfs" subvolume list "$TEST_MNT" | \
	cut -d\  -f9 > "$tmp/output"
run_check_umount_test_dev

result=$(cat "$tmp/output")

if [ "$result" != "dir/subvol" ]; then
	_fail "dir/subvol not in subvolume list"
fi

rm -rf -- "$tmp"
