#!/bin/bash
# Test various block group profile combinations for remap-tree.
# This test is based on mkfs-tests/039-zoned-profiles .

source "$TEST_TOP/common" || exit

check_prereq mkfs.btrfs
check_prereq btrfs

_not_run "btrfs check support for remap-tree missing"

setup_root_helper
setup_loopdevs 4
prepare_loopdevs
TEST_DEV=${loopdevs[1]}
dev1=${loopdevs[1]}

test_get_info()
{
	local tmp_out

	tmp_out=$(_mktemp mkfs-get-info)
	run_check $SUDO_HELPER "$TOP/btrfs" inspect-internal dump-super "$dev1"
	run_check $SUDO_HELPER "$TOP/btrfs" check "$dev1"

	run_check $SUDO_HELPER mount -o ro "$dev1" "$TEST_MNT"
	run_check_stdout "$TOP/btrfs" filesystem df "$TEST_MNT" > "$tmp_out"
	if grep -q "Multiple block group profiles detected" "$tmp_out"; then
		rm -- "$tmp_out"
		_fail "temporary chunks are not properly cleaned up"
	fi
	rm -- "$tmp_out"
	run_check $SUDO_HELPER "$TOP/btrfs" filesystem usage "$TEST_MNT"
	run_check $SUDO_HELPER "$TOP/btrfs" device usage "$TEST_MNT"
	run_check $SUDO_HELPER umount "$TEST_MNT"
}

test_do_mkfs()
{
	run_check $SUDO_HELPER "$TOP/mkfs.btrfs" -f -O remap-tree "$@"
	if run_check_stdout $SUDO_HELPER "$TOP/btrfs" check "$dev1" | grep -iq warning; then
		_fail "warnings found in check output"
	fi
}

test_mkfs_single()
{
	test_do_mkfs "$@" "$dev1"
	test_get_info
}

test_mkfs_multi()
{
	test_do_mkfs "$@" "${loopdevs[@]}"
	test_get_info
}

test_mkfs_single
test_mkfs_single  -d  single  -m  single
test_mkfs_single  -d  single  -m  dup

test_mkfs_multi
test_mkfs_multi   -d  single  -m  single

if _test_config "EXPERIMENTAL" && [ -f "/sys/fs/btrfs/features/remap_tree" ]; then
	test_mkfs_single  -d  dup     -m  single
	test_mkfs_single  -d  dup     -m  dup

	test_mkfs_multi   -d  raid0   -m  raid0
	test_mkfs_multi   -d  raid1   -m  raid1
	test_mkfs_multi   -d  raid10  -m  raid10
	test_mkfs_multi   -d  raid5   -m  raid5
	test_mkfs_multi   -d  raid6   -m  raid6
	test_mkfs_multi   -d  dup     -m  dup

	if [ -f "/sys/fs/btrfs/features/raid1c34" ]; then
		test_mkfs_multi   -d  raid1c3 -m  raid1c3
		test_mkfs_multi   -d  raid1c4 -m  raid1c4
	else
		_log "skip mount test, missing support for raid1c34"
		test_do_mkfs -d raid1c3 -m raid1c3 "${loopdevs[@]}"
		test_do_mkfs -d raid1c4 -m raid1c4 "${loopdevs[@]}"
	fi

	# Non-standard profile/device combinations

	# Single device raid0, two device raid10 (simple mount works on older kernels too)
	test_do_mkfs -d raid0 -m raid0 "$dev1"
	test_get_info
	test_do_mkfs -d raid10 -m raid10 "${loopdevs[1]}" "${loopdevs[2]}"
	test_get_info
fi

cleanup_loopdevs
