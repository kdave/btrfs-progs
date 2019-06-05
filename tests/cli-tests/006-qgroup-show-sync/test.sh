#!/bin/bash
#
# simple test of qgroup show --sync option

source "$TEST_TOP/common"

check_prereq mkfs.btrfs
check_prereq btrfs

setup_root_helper
prepare_test_dev 1g

run_check_mkfs_test_dev
run_check_mount_test_dev

run_check $SUDO_HELPER "$TOP/btrfs" subvolume create "$TEST_MNT/Sub"
run_check $SUDO_HELPER "$TOP/btrfs" quota enable "$TEST_MNT/Sub"

for opt in '' '--' '--sync'; do
	run_check $SUDO_HELPER "$TOP/btrfs" qgroup limit 300M "$TEST_MNT/Sub"
	run_check $SUDO_HELPER dd if=/dev/zero of="$TEST_MNT/Sub/file" bs=1M count=200

	run_check $SUDO_HELPER "$TOP/btrfs" qgroup show -re $opt "$TEST_MNT/Sub"

	run_check $SUDO_HELPER "$TOP/btrfs" qgroup limit none "$TEST_MNT/Sub"
	run_check $SUDO_HELPER rm -f "$TEST_MNT/Sub/file"
	run_check "$TOP/btrfs" filesystem sync "$TEST_MNT/Sub"
done

run_check_umount_test_dev
