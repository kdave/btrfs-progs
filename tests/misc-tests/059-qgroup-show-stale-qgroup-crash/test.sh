#!/bin/bash
# Create a stale qgroup and check that 'qgroup show' does not crash when trying
# to print the path

source "$TEST_TOP/common" || exit

setup_root_helper
prepare_test_dev

run_check_mkfs_test_dev
run_check_mount_test_dev
count=24
run_check $SUDO_HELPER dd if=/dev/zero of="$TEST_MNT"/file bs=1M count=1
run_check $SUDO_HELPER "$TOP/btrfs" quota enable "$TEST_MNT"
for i in `seq $count`; do
	run_check $SUDO_HELPER "$TOP/btrfs" subvolume create "$TEST_MNT/subv$i"
	if [ "$(($i % 2))" = "0" ]; then
		run_check $SUDO_HELPER "$TOP/btrfs" subvolume delete "$TEST_MNT/subv$i"
	fi
done
run_check $SUDO_HELPER "$TOP/btrfs" qgroup show --sort path "$TEST_MNT"
run_check_umount_test_dev
