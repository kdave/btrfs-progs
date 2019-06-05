#!/bin/bash
# make sure that 'missing' is accepted for device deletion

source "$TEST_TOP/common"

check_prereq mkfs.btrfs
check_prereq btrfs

setup_root_helper

test_do_mkfs()
{
	run_check $SUDO_HELPER "$TOP/mkfs.btrfs" -f "$@" ${loopdevs[@]}
	run_check $SUDO_HELPER "$TOP/btrfs" inspect-internal dump-super "$dev1"
	run_check $SUDO_HELPER "$TOP/btrfs" check "$dev1"
	run_check $SUDO_HELPER "$TOP/btrfs" filesystem show
}

test_wipefs()
{
	run_check $SUDO_HELPER wipefs -a "$devtodel"
	run_check $SUDO_HELPER losetup -d "$devtodel"
	unset loopdevs[3]
	run_check $SUDO_HELPER losetup --all
	run_check "$TOP/btrfs" filesystem show
}
test_delete_missing()
{
	run_check_mount_test_dev -o degraded
	run_check $SUDO_HELPER "$TOP/btrfs" filesystem show "$TEST_MNT"
	run_check $SUDO_HELPER "$TOP/btrfs" device delete missing "$TEST_MNT"
	run_check $SUDO_HELPER "$TOP/btrfs" filesystem show "$TEST_MNT"
	run_check_umount_test_dev

	run_check_mount_test_dev
	local out
	out=$(run_check_stdout $SUDO_HELPER "$TOP/btrfs" filesystem show "$TEST_MNT")
	if echo "$out" | grep -q -- "$devtodel"; then
		_fail "device $devtodel not deleted"
	fi
	if echo "$out" | grep -q missing; then
		_fail "missing device still present"
	fi
	run_check_umount_test_dev
}

test_missing_error()
{
	local out

	run_check_mkfs_test_dev
	run_check_mount_test_dev
	out=$(run_mustfail_stdout "device remove succeeded" \
		$SUDO_HELPER "$TOP/btrfs" device remove missing "$TEST_MNT")

	if ! echo "$out" | grep -q "no missing devices found to remove"; then
		_fail "IOCTL returned unexpected error value"
	fi

	run_check_umount_test_dev
}

setup_loopdevs 4
prepare_loopdevs
dev1=${loopdevs[1]}
devtodel=${loopdevs[3]}
TEST_DEV=$dev1

test_do_mkfs -m raid1 -d raid1
test_wipefs
test_delete_missing
test_missing_error

cleanup_loopdevs
