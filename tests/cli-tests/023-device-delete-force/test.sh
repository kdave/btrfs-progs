#!/bin/bash
# Test multiple devices for removal, timeout and warning

source "$TEST_TOP/common" || exit

setup_root_helper
setup_loopdevs 4
prepare_loopdevs
TEST_DEV=${loopdevs[1]}

# Print warning and wait
run_check $SUDO_HELPER "$TOP/mkfs.btrfs" -f -d raid1 -m raid1 "${loopdevs[@]}"
run_check_mount_test_dev
run_check $SUDO_HELPER dd if=/dev/zero of="$TEST_MNT"/file bs=100M count=1
run_check "$TOP/btrfs" filesystem usage -T "$TEST_MNT"
run_check $SUDO_HELPER "$TOP/btrfs" device delete "${loopdevs[3]}" "${loopdevs[2]}" "$TEST_MNT"
run_check "$TOP/btrfs" filesystem usage -T "$TEST_MNT"
run_check_umount_test_dev

# Print warning and skip waiting
run_check $SUDO_HELPER "$TOP/mkfs.btrfs" -f -d raid1 -m raid1 "${loopdevs[@]}"
run_check_mount_test_dev
run_check $SUDO_HELPER dd if=/dev/zero of="$TEST_MNT"/file bs=100M count=1
run_check "$TOP/btrfs" filesystem usage -T "$TEST_MNT"
run_check $SUDO_HELPER "$TOP/btrfs" device delete --force "${loopdevs[3]}" "${loopdevs[2]}" "$TEST_MNT"
run_check "$TOP/btrfs" filesystem usage -T "$TEST_MNT"
run_check_umount_test_dev

cleanup_loopdevs
