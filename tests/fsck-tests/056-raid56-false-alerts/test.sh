#!/bin/bash
#
# Make sure "btrfs check --check-data-csum" won't report false alerts on RAID56
# data.
#

source "$TEST_TOP/common"

check_prereq btrfs
check_prereq mkfs.btrfs
check_global_prereq losetup

setup_loopdevs 3
prepare_loopdevs
dev1=${loopdevs[1]}
TEST_DEV=$dev1

setup_root_helper

run_check $SUDO_HELPER "$TOP/mkfs.btrfs" -f -m raid1 -d raid5 "${loopdevs[@]}"
run_check_mount_test_dev

run_check $SUDO_HELPER dd if=/dev/urandom of="$TEST_MNT/file" bs=16K count=1 \
	status=noxfer > /dev/null 2>&1

run_check_umount_test_dev

# Check data csum should not report false alerts
run_check "$TOP/btrfs" check --check-data-csum "$dev1"

cleanup_loopdevs
