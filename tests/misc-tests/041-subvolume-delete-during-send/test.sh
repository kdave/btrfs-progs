#!/bin/bash
# Detect if subvolume deletion fails when it's part of send

source "$TEST_TOP/common" || exit

check_prereq mkfs.btrfs
check_prereq btrfs

setup_root_helper
prepare_test_dev 4G

run_check_mkfs_test_dev
run_check_mount_test_dev

run_check $SUDO_HELPER "$TOP/btrfs" subvolume create "$TEST_MNT/subv1"
# Generate 128M of data to send, far more than the pipe buffers can hold
run_check $SUDO_HELPER dd if=/dev/zero of="$TEST_MNT/subv1/file" bs=1M count=128
run_check $SUDO_HELPER "$TOP/btrfs" subvolume snapshot -r "$TEST_MNT/subv1" "$TEST_MNT/snap1"

run_check "$TOP/btrfs" filesystem sync "$TEST_MNT"
# Send blocks on a full pipe until the reader starts draining it, which is
# later than the subvolume deletion below
$SUDO_HELPER "$TOP/btrfs" send "$TEST_MNT/snap1" 2>> "$RESULTS" |
	{ sleep 4; cat > /dev/null; } &
# Give send time to start
run_check sleep 2

# Fail if send is still in progress
run_mustfail "deleting default subvolume by path succeeded" \
	$SUDO_HELPER "$TOP/btrfs" subvolume delete "$TEST_MNT/snap1"

# Wait for send
wait

run_check_umount_test_dev
