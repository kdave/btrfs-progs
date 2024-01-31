#!/bin/bash
# Create subvolumes with enabled qutoas and check that subvolume deleteion will
# also delete the 0-level qgruop.

source "$TEST_TOP/common" || exit

setup_root_helper
prepare_test_dev

run_check_mkfs_test_dev
run_check_mount_test_dev
run_check $SUDO_HELPER dd if=/dev/zero of="$TEST_MNT"/file bs=1M count=1

# Without quotas
run_check $SUDO_HELPER "$TOP/btrfs" subvolume create "$TEST_MNT/subv1"
run_check $SUDO_HELPER "$TOP/btrfs" subvolume create "$TEST_MNT/subv2"
run_check $SUDO_HELPER "$TOP/btrfs" subvolume delete --delete-qgroup "$TEST_MNT/subv1"
run_check $SUDO_HELPER "$TOP/btrfs" subvolume delete --no-delete-qgroup "$TEST_MNT/subv2"
run_check $SUDO_HELPER "$TOP/btrfs" filesystem sync "$TEST_MNT"
run_check $SUDO_HELPER "$TOP/btrfs" subvolume sync "$TEST_MNT"
run_check $SUDO_HELPER "$TOP/btrfs" subvol list "$TEST_MNT"

# With quotas enabled
run_check $SUDO_HELPER "$TOP/btrfs" quota enable "$TEST_MNT"
run_check $SUDO_HELPER "$TOP/btrfs" subvolume create "$TEST_MNT/subv1"
rootid1=$(run_check_stdout "$TOP/btrfs" inspect-internal rootid "$TEST_MNT/subv1")
run_check $SUDO_HELPER "$TOP/btrfs" subvolume create "$TEST_MNT/subv2"
rootid2=$(run_check_stdout "$TOP/btrfs" inspect-internal rootid "$TEST_MNT/subv2")
run_check $SUDO_HELPER "$TOP/btrfs" qgroup create 1/1 "$TEST_MNT"
run_check $SUDO_HELPER "$TOP/btrfs" qgroup assign "0/$rootid1" 1/1 "$TEST_MNT"
run_check $SUDO_HELPER "$TOP/btrfs" qgroup assign "0/$rootid2" 1/1 "$TEST_MNT"
run_check $SUDO_HELPER "$TOP/btrfs" quota rescan --wait "$TEST_MNT"
run_check $SUDO_HELPER "$TOP/btrfs" subvol list "$TEST_MNT"
run_check $SUDO_HELPER "$TOP/btrfs" qgroup show "$TEST_MNT"
run_check $SUDO_HELPER "$TOP/btrfs" subvolume delete --delete-qgroup "$TEST_MNT/subv1"
run_check $SUDO_HELPER "$TOP/btrfs" subvolume delete --no-delete-qgroup "$TEST_MNT/subv2"
run_check $SUDO_HELPER "$TOP/btrfs" filesystem sync "$TEST_MNT"
run_check $SUDO_HELPER "$TOP/btrfs" subvolume sync "$TEST_MNT"
run_check $SUDO_HELPER "$TOP/btrfs" subvol list "$TEST_MNT"
run_check $SUDO_HELPER "$TOP/btrfs" qgroup show "$TEST_MNT"
if run_check_stdout $SUDO_HELPER "$TOP/btrfs" qgroup show "$TEST_MNT" | grep -q "0/$rootid1"; then
	_fail "qgroup 0/$rootid1 not deleted"
fi
if ! run_check_stdout $SUDO_HELPER "$TOP/btrfs" qgroup show "$TEST_MNT" | grep -q "0/$rootid2"; then
	_fail "qgroup 0/$rootid2 deleted"
fi
run_check_umount_test_dev
