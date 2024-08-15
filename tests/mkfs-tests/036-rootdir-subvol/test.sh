#!/bin/bash
# Basic test for mkfs.btrfs --subvol option

source "$TEST_TOP/common" || exit

check_prereq mkfs.btrfs
check_prereq btrfs

setup_root_helper
prepare_test_dev

tmp=$(_mktemp_dir mkfs-rootdir)

basic()
{
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
	rm -rf -- "$tmp/foo" "$tmp/dir"
}

basic_hardlinks()
{
	run_check touch "$tmp/hl1"
	run_check ln "$tmp/hl1" "$tmp/hl2"
	run_check mkdir "$tmp/dir"
	run_check ln "$tmp/hl1" "$tmp/dir/hl3"

	run_check_mkfs_test_dev --rootdir "$tmp"
	run_check $SUDO_HELPER "$TOP/btrfs" check "$TEST_DEV"

	run_check_mount_test_dev
	nr_hardlink=$(run_check_stdout $SUDO_HELPER stat -c "%h" "$TEST_MNT/hl1")

	if [ "$nr_hardlink" -ne 3 ]; then
		_fail "hard link number incorrect, has ${nr_hardlink} expect 3"
	fi
	run_check_umount_test_dev
	rm -rf -- "$tmp/hl1" "$tmp/hl2" "$tmp/dir"
}

split_by_subvolume_hardlinks()
{
	run_check touch "$tmp/hl1"
	run_check ln "$tmp/hl1" "$tmp/hl2"
	run_check mkdir "$tmp/subv"
	run_check ln "$tmp/hl1" "$tmp/subv/hl3"

	run_check_mkfs_test_dev --rootdir "$tmp" --subvol subv
	run_check $SUDO_HELPER "$TOP/btrfs" check "$TEST_DEV"

	run_check_mount_test_dev
	nr_hardlink=$(run_check_stdout $SUDO_HELPER stat -c "%h" "$TEST_MNT/hl1")

	if [ "$nr_hardlink" -ne 2 ]; then
		_fail "hard link number incorrect for hl1, has ${nr_hardlink} expect 2"
	fi

	nr_hardlink=$(run_check_stdout $SUDO_HELPER stat -c "%h" "$TEST_MNT/subv/hl3")
	if [ "$nr_hardlink" -ne 1 ]; then
		_fail "hard link number incorrect for subv/hl3, has ${nr_hardlink} expect 1"
	fi
	run_check_umount_test_dev
	rm -rf -- "$tmp/hl1" "$tmp/hl2" "$tmp/dir"
}

basic
basic_hardlinks
split_by_subvolume_hardlinks
rm -rf -- "$tmp"
