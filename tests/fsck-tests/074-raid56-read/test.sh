#!/bin/bash
#
# Make sure "btrfs check" can properly handle RAID5 data reconstruction.

source "$TEST_TOP/common" || exit

check_prereq btrfs
check_prereq mkfs.btrfs
check_prereq fsstress
check_global_prereq fallocate

setup_root_helper
setup_loopdevs 3
prepare_loopdevs
dev1=${loopdevs[1]}
TEST_DEV=$dev1
fsstress_prog="$INTERNAL_BIN/fsstress"

run_check $SUDO_HELPER "$TOP/mkfs.btrfs" -f -m raid1 -d raid5 "${loopdevs[@]}"
run_check_mount_test_dev
run_check $SUDO_HELPER $fsstress_prog -w -n 256 -d "$TEST_MNT"
run_check_umount_test_dev

# Check data csum should not report false alerts
run_check $SUDO_HELPER "$TOP/btrfs" check --check-data-csum "$dev1"

cleanup_loopdevs
