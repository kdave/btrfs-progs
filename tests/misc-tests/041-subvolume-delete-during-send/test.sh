#!/bin/bash
# Detect if subvolume deletion fails when it's part of send

source "$TEST_TOP/common"

check_prereq mkfs.btrfs
check_prereq btrfs

prepare_test_dev 4G

run_check_mkfs_test_dev
run_check_mount_test_dev

run_check $SUDO_HELPER "$TOP/btrfs" subvolume create "$TEST_MNT/subv1"
# Generate 2.7G of data to send, should be slow enough to let the subvolume
# deletion catch send in progress
for i in `seq 10`; do
	run_check $SUDO_HELPER dd if=/dev/zero of="$TEST_MNT/subv1/file$i" bs=50M count="$i"
done
run_check $SUDO_HELPER "$TOP/btrfs" subvolume snapshot -r "$TEST_MNT/subv1" "$TEST_MNT/snap1"

stream="stream$RANDOM.out"
rm -f -- "$stream"
touch -- "$stream"
chmod a+rw -- "$stream"
run_check "$TOP/btrfs" filesystem sync "$TEST_MNT"
# Output to file must be slow
run_check $SUDO_HELPER "$TOP/btrfs" send -f "$stream" "$TEST_MNT/snap1" &
# Give send time to start
run_check sleep 2
# If this fails, send was fast
ps faux | run_check grep "btrfs send -f $stream"

# Fail if send is still in progress, may not work on a really fast device
run_mustfail "deleting default subvolume by path succeeded" \
	$SUDO_HELPER "$TOP/btrfs" subvolume delete "$TEST_MNT/snap1"

# Wait for send
wait
rm -f -- "$stream"

run_check_umount_test_dev
