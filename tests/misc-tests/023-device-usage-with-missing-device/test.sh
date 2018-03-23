#!/bin/bash
# check if 'device slack' is reported as zero when a device is missing

source "$TEST_TOP/common"

check_prereq btrfs-image
check_prereq mkfs.btrfs
check_prereq btrfs

setup_root_helper

test_run()
{
	# empty filesystem, with enough redundancy so degraded mount works
	run_check $SUDO_HELPER "$TOP/mkfs.btrfs" -f -d raid1 -m raid1 "$dev1" "$dev2"

	TEST_DEV="$dev1"
	run_check_mount_test_dev
	run_check $SUDO_HELPER "$TOP/btrfs" filesystem usage "$TEST_MNT"
	run_check_stdout $SUDO_HELPER "$TOP/btrfs" device usage "$TEST_MNT" | \
		grep -q "slack.*16\\.00EiB" && _fail
	run_check_umount_test_dev

	run_mayfail wipefs -a "$dev2"
	run_check $SUDO_HELPER losetup -d "$dev2"
	unset loopdevs[2]

	run_check_mount_test_dev -o degraded,ro
	run_check $SUDO_HELPER "$TOP/btrfs" filesystem usage "$TEST_MNT"
	run_check_stdout $SUDO_HELPER "$TOP/btrfs" device usage "$TEST_MNT" | \
		grep -q "slack.*16\\.00EiB" && _fail
	run_check_umount_test_dev
}

setup_loopdevs 2
prepare_loopdevs
dev1=${loopdevs[1]}
dev2=${loopdevs[2]}
test_run
cleanup_loopdevs
