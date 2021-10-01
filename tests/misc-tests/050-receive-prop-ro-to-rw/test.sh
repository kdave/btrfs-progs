#!/bin/bash
# Prevent changing subvolume ro->rw status with received_uuid set, unless forced

source "$TEST_TOP/common"

setup_root_helper
prepare_test_dev

run_check_mkfs_test_dev
run_check_mount_test_dev

run_check $SUDO_HELPER mkdir "$TEST_MNT/src" "$TEST_MNT/dst"
run_check $SUDO_HELPER "$TOP/btrfs" subvolume create "$TEST_MNT/src/subvol1"
for i in `seq 10`; do
	run_check $SUDO_HELPER dd if=/dev/zero of="$TEST_MNT/file$1" bs=10K count=1
done
run_check $SUDO_HELPER "$TOP/btrfs" subvolume snapshot -r "$TEST_MNT/src/subvol1" "$TEST_MNT/src/snap1"

for i in `seq 10`; do
	run_check $SUDO_HELPER dd if=/dev/zero of="$TEST_MNT/file-new$1" bs=1K count=1
done
run_check $SUDO_HELPER "$TOP/btrfs" subvolume snapshot -r "$TEST_MNT/src/subvol1" "$TEST_MNT/src/snap2"

touch send1.stream send2.stream
chmod a+w send1.stream send2.stream
run_check $SUDO_HELPER "$TOP/btrfs" send -f send1.stream "$TEST_MNT/src/snap1"
run_check $SUDO_HELPER "$TOP/btrfs" send -f send2.stream -p "$TEST_MNT/src/snap1" "$TEST_MNT/src/snap2"

run_check $SUDO_HELPER "$TOP/btrfs" receive -f send1.stream -m "$TEST_MNT" "$TEST_MNT/dst"
run_check $SUDO_HELPER "$TOP/btrfs" receive -f send2.stream -m "$TEST_MNT" "$TEST_MNT/dst"

run_check $SUDO_HELPER "$TOP/btrfs" subvolume show "$TEST_MNT/dst/snap2"
run_check $SUDO_HELPER "$TOP/btrfs" property get "$TEST_MNT/dst/snap2" ro
run_mustfail "ro->rw switch and received_uuid not reset" \
	$SUDO_HELPER "$TOP/btrfs" property set "$TEST_MNT/dst/snap2" ro false

run_check $SUDO_HELPER "$TOP/btrfs" property set -f "$TEST_MNT/dst/snap2" ro false
run_check $SUDO_HELPER "$TOP/btrfs" subvolume show "$TEST_MNT/dst/snap2"

run_check_umount_test_dev

rm -f send1.stream send2.stream
