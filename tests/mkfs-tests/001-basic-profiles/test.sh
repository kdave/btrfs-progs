#!/bin/bash
# test various blockgroup profile combinations, use loop devices as block
# devices

source "$TEST_TOP/common"

check_prereq mkfs.btrfs
check_prereq btrfs

setup_root_helper

test_get_info()
{
	local tmp_out

	tmp_out=$(_mktemp mkfs-get-info)
	run_check $SUDO_HELPER "$TOP/btrfs" inspect-internal dump-super "$dev1"
	run_check $SUDO_HELPER "$TOP/btrfs" check "$dev1"

	# Work around for kernel bug that will treat SINGLE and single
	# device RAID0 as the same.
	# Thus kernel may create new SINGLE chunks, causing extra warning
	# when testing single device RAID0.
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
	run_check $SUDO_HELPER "$TOP/mkfs.btrfs" -f "$@"
	if run_check_stdout "$TOP/btrfs" check "$dev1" | grep -iq warning; then
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
	test_do_mkfs "$@" ${loopdevs[@]}
	test_get_info
}

setup_loopdevs 4
prepare_loopdevs
dev1=${loopdevs[1]}

test_mkfs_single
test_mkfs_single  -d  single  -m  single
test_mkfs_single  -d  single  -m  single  --mixed
test_mkfs_single  -d  single  -m  dup
test_mkfs_single  -d  dup     -m  single
test_mkfs_single  -d  dup     -m  dup
test_mkfs_single  -d  dup     -m  dup     --mixed

test_mkfs_multi
test_mkfs_multi   -d  single  -m  single
test_mkfs_multi   -d  single  -m  single  --mixed
test_mkfs_multi   -d  raid0   -m  raid0
test_mkfs_multi   -d  raid0   -m  raid0   --mixed
test_mkfs_multi   -d  raid1   -m  raid1
test_mkfs_multi   -d  raid1   -m  raid1   --mixed
test_mkfs_multi   -d  raid10  -m  raid10
test_mkfs_multi   -d  raid10  -m  raid10  --mixed
test_mkfs_multi   -d  raid5   -m  raid5
test_mkfs_multi   -d  raid5   -m  raid5   --mixed
test_mkfs_multi   -d  raid6   -m  raid6
test_mkfs_multi   -d  raid6   -m  raid6   --mixed
test_mkfs_multi   -d  dup     -m  dup
test_mkfs_multi   -d  dup     -m  dup     --mixed

if [ -f "/sys/fs/btrfs/features/raid1c34" ]; then
	test_mkfs_multi   -d  raid1c3 -m  raid1c3
	test_mkfs_multi   -d  raid1c3 -m  raid1c3 --mixed
	test_mkfs_multi   -d  raid1c4 -m  raid1c4
	test_mkfs_multi   -d  raid1c4 -m  raid1c4 --mixed
else
	_log "skip mount test, missing support for raid1c34"
	test_do_mkfs -d raid1c3 -m raid1c3 ${loopdevs[@]}
	test_do_mkfs -d raid1c4 -m raid1c4 ${loopdevs[@]}
fi

# Non-standard profile/device combinations

# Single device raid0, two device raid10 (simple mount works on older kernels too)
if check_min_kernel_version 5.13; then
	test_do_mkfs -d raid0 -m raid0 "$dev1"
	test_get_info
	test_do_mkfs -d raid10 -m raid10 "${loopdevs[1]}" "${loopdevs[2]}"
	test_get_info
fi

cleanup_loopdevs
