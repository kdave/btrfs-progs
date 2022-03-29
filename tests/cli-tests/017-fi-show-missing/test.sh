#!/bin/bash
#
# Test that if a device is missing for a mounted filesystem, btrfs fi show will
# show which device exactly is missing.

source "$TEST_TOP/common"

check_prereq mkfs.btrfs
check_prereq btrfs

setup_root_helper
setup_loopdevs 2
prepare_loopdevs

dev1=${loopdevs[1]}
dev2=${loopdevs[2]}

run_check $SUDO_HELPER "$TOP/mkfs.btrfs" -f -d raid1 "${loopdevs[@]}"

# Move the device, changing its path, simulating the device being missing
run_check $SUDO_HELPER mv "$dev2" /dev/loop-non-existent

run_check $SUDO_HELPER mount -o degraded $dev1 $TEST_MNT

if ! run_check_stdout $SUDO_HELPER "$TOP/btrfs" filesystem show "$TEST_MNT" | \
	grep -q "$dev2 MISSING"; then

	_fail "didn't find exact missing device"
fi

run_check $SUDO_HELPER mv /dev/loop-non-existent "$dev2"

run_check $SUDO_HELPER umount "$TEST_MNT"

cleanup_loopdevs
