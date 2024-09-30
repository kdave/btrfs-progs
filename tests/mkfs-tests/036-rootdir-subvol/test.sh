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

	if [ "$1" != "" ]; then
		run_check_mkfs_test_dev --rootdir "$tmp" --subvol $1:dir/subvol
	else
		run_check_mkfs_test_dev --rootdir "$tmp" --subvol dir/subvol
	fi

	run_check $SUDO_HELPER "$TOP/btrfs" check "$TEST_DEV"

	run_check_mount_test_dev -o subvolid=5
	run_check_stdout $SUDO_HELPER "$TOP/btrfs" subvolume list "$TEST_MNT" | \
	cut -d\  -f9 > "$tmp/output"
	run_check_stdout "$TOP/btrfs" property get "$TEST_MNT/dir/subvol" ro | \
	cut -d = -f2 > "$tmp/output2"
	run_check_stdout "$TOP/btrfs" subvolume get-default "$TEST_MNT" | \
	cut -d\  -f2 > "$tmp/output3"
	run_check_umount_test_dev

	result=$(cat "$tmp/output")

	if [ "$result" != "dir/subvol" ]; then
		_fail "dir/subvol not in subvolume list"
	fi

	result=$(cat "$tmp/output2")

	if [ "$1" == "ro" -o "$1" == "default-ro" ]; then
		if [ "$result" != "true" ]; then
			 _fail "dir/subvol was read-write, expected read-only"
		fi
	else
		if [ "$result" != "false" ]; then
			_fail "dir/subvol was read-only, expected read-write"
		fi
	fi

	result=$(cat "$tmp/output3")

	if [ "$1" == "default" -o "$1" == "default-ro" ]; then
		if [ "$result" != "256" ]; then
			 _fail "default subvol was $result, expected 256"
		fi
	else
		if [ "$result" != "5" ]; then
			_fail "default subvol was $result, expected 5"
		fi
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

	if [ "$1" != "" ]; then
		run_check_mkfs_test_dev --rootdir "$tmp" --subvol $1:subv
	else
		run_check_mkfs_test_dev --rootdir "$tmp" --subvol subv
	fi

	run_check $SUDO_HELPER "$TOP/btrfs" check "$TEST_DEV"

	run_check_mount_test_dev -o subvolid=5
	nr_hardlink=$(run_check_stdout $SUDO_HELPER stat -c "%h" "$TEST_MNT/hl1")

	if [ "$nr_hardlink" -ne 2 ]; then
		_fail "hard link number incorrect for hl1, has ${nr_hardlink} expect 2"
	fi

	nr_hardlink=$(run_check_stdout $SUDO_HELPER stat -c "%h" "$TEST_MNT/subv/hl3")
	if [ "$nr_hardlink" -ne 1 ]; then
		_fail "hard link number incorrect for subv/hl3, has ${nr_hardlink} expect 1"
	fi
	run_check_umount_test_dev
	rm -rf -- "$tmp/hl1" "$tmp/hl2" "$tmp/subv"
}

for mod in "" ro rw default default-ro;
do
	basic $mod
	split_by_subvolume_hardlinks $mod
done

basic_hardlinks
rm -rf -- "$tmp"
