#!/bin/bash
# Verify that raid56 warning is printed before balance conversion when the
# target profile is raid5 or raid6, but not other profiles

source "$TEST_TOP/common"

check_prereq mkfs.btrfs
check_prereq btrfs

setup_root_helper
setup_loopdevs 6
prepare_loopdevs

TEST_DEV=${loopdevs[1]}

run_check $SUDO_HELPER "$TOP/mkfs.btrfs" -f -d single -m single "${loopdevs[@]}"
run_check_mount_test_dev
stdout=$(run_check_stdout $SUDO_HELPER "$TOP/btrfs" balance start -mconvert=raid5 "$TEST_MNT")
if ! echo "$stdout" | grep -q WARNING; then
	_fail "no warning for balance conversion"
fi
run_check $SUDO_HELPER "$TOP/btrfs" balance start -f -mconvert=raid6 "$TEST_MNT"
run_mayfail $SUDO_HELPER "$TOP/btrfs" balance start -mconvert=raid0 "$TEST_MNT"
run_check $SUDO_HELPER "$TOP/btrfs" balance start -f -mconvert=raid0 "$TEST_MNT"
if run_check_stdout $SUDO_HELPER "$TOP/btrfs" balance start -mconvert=raid1c3 "$TEST_MNT" |
		grep -q WARNING; then
	_fail "warning found for balance conversion"
fi
if run_check_stdout $SUDO_HELPER "$TOP/btrfs" balance start -mconvert=raid1c4 "$TEST_MNT" |
		grep -q WARNING; then
	_fail "warning found for balance conversion"
fi
run_check_umount_test_dev

cleanup_loopdevs
