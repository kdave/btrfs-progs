#!/bin/bash
# All tests for commands that support global option --dry-run:
#
# - subvolume delete

source "$TEST_TOP/common" || exit

setup_root_helper
setup_loopdevs 4
prepare_loopdevs

TEST_DEV=${loopdevs[1]}

sleep 1

run_check $SUDO_HELPER "$TOP/mkfs.btrfs" -f -d raid1 -m raid1 "${loopdevs[@]}"
run_check_mount_test_dev
run_check $SUDO_HELPER "$TOP/btrfs" subvolume create "$TEST_MNT/subv1"
run_check $SUDO_HELPER "$TOP/btrfs" subvolume create "$TEST_MNT/subv2"
run_check $SUDO_HELPER "$TOP/btrfs" subvolume create "$TEST_MNT/subv3"
run_check $SUDO_HELPER "$TOP/btrfs" subvolume create "$TEST_MNT/subv4"

# Delete 2 without --dry-run
run_check $SUDO_HELPER "$TOP/btrfs" subvolume delete "$TEST_MNT/subv2"
# Delete all with --dry-run
run_check $SUDO_HELPER "$TOP/btrfs" --dry-run subvolume delete "$TEST_MNT/subv"*
run_check $SUDO_HELPER "$TOP/btrfs" --dry-run subvolume delete --commit-after "$TEST_MNT/subv"*
run_check $SUDO_HELPER "$TOP/btrfs" --dry-run subvolume delete --commit-each "$TEST_MNT/subv"*

run_check $SUDO_HELPER ls -lid "$TEST_MNT/subv"*
run_check $SUDO_HELPER stat "$TEST_MNT/subv1"
run_mustfail "subv2 should not exist" $SUDO_HELPER stat "$TEST_MNT/subv2"
run_check $SUDO_HELPER stat "$TEST_MNT/subv3"
run_check $SUDO_HELPER stat "$TEST_MNT/subv4"
run_check_umount_test_dev

cleanup_loopdevs
