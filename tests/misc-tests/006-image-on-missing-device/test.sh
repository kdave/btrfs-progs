#!/bin/bash
# test btrfs-image with a missing device (uses loop devices)
#
# - btrfs-image must not loop indefinitely
# - btrfs-image will expectedly fail to produce the dump

source "$TEST_TOP/common"

check_prereq btrfs-image
check_prereq mkfs.btrfs
check_prereq btrfs

setup_root_helper

test_image_dump()
{
	run_check $SUDO_HELPER "$TOP/btrfs" check "$dev1"
	# the output file will be deleted
	run_mayfail $SUDO_HELPER "$TOP/btrfs-image" "$dev1" /tmp/test-img.dump
}

test_run()
{
	run_check $SUDO_HELPER "$TOP/mkfs.btrfs" -f -d raid1 -m raid1 "$dev1" "$dev2"

	# we need extents to trigger reading from all devices
	run_check $SUDO_HELPER mount "$dev1" "$TEST_MNT"
	run_check $SUDO_HELPER dd if=/dev/zero of="$TEST_MNT/a" bs=1M count=10
	run_check $SUDO_HELPER dd if=/dev/zero of="$TEST_MNT/b" bs=4k count=1000 conv=sync
	run_check $SUDO_HELPER umount "$TEST_MNT"

	test_image_dump
	run_check $SUDO_HELPER "$TOP/btrfs" filesystem show "$dev1"
	# create a degraded raid1 filesystem, check must succeed
	# btrfs-image must not loop
	run_mayfail wipefs -a "$dev2"
	run_check $SUDO_HELPER losetup -d "$dev2"
	unset loopdevs[2]
	run_check $SUDO_HELPER "$TOP/btrfs" filesystem show "$dev1"

	test_image_dump
}

setup_loopdevs 2
prepare_loopdevs
dev1=${loopdevs[1]}
dev2=${loopdevs[2]}
test_run
cleanup_loopdevs
