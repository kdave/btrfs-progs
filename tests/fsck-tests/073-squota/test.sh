#!/bin/bash
#
# Make sure "btrfs check" can handle EXTENT_OWNER_REF key type for squota
#

source "$TEST_TOP/common" || exit

check_prereq btrfs
check_prereq mkfs.btrfs
check_global_prereq fallocate

if [ ! -f /sys/fs/btrfs/features/simple_quota ] ; then
	_not_run "no kernel simple quota support"
fi

setup_root_helper
prepare_test_dev

run_check_mkfs_test_dev -O squota
run_check_mount_test_dev
run_check $SUDO_HELPER "$TOP/btrfs" subvolume create "$TEST_MNT/subv1"
run_check $SUDO_HELPER dd if=/dev/zero bs=1M count=1 of="$TEST_MNT/subv1/regular"
run_check $SUDO_HELPER fallocate -l 8m "$TEST_MNT/subv1/preallocated"
for ((i = 0; i < 64; i++)); do
	run_check $SUDO_HELPER dd if=/dev/urandom bs=1K count=1 of="$TEST_MNT/subv1/inline_$i"
done

run_check $SUDO_HELPER "$TOP/btrfs" subvolume snapshot "$TEST_MNT/subv1" "$TEST_MNT/snap1"
run_check_umount_test_dev

run_check $SUDO_HELPER "$TOP/btrfs" check "$TEST_DEV"
