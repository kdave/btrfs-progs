#!/bin/bash
# Check that an enqueued replace over another replace will wait

source "$TEST_TOP/common" || exit

check_prereq mkfs.btrfs
check_prereq btrfs

setup_root_helper
setup_loopdevs 6
prepare_loopdevs

TEST_DEV=${loopdevs[1]}
REPLACE1=${loopdevs[6]}
REPLACE2=${loopdevs[5]}

# Use striped profile so all devices are at least partially filled
run_check $SUDO_HELPER "$TOP/mkfs.btrfs" -f -d raid0 -m raid0 "${loopdevs[@]}"
run_check_mount_test_dev
# Remove the devices and then use them for replace
run_check $SUDO_HELPER "$TOP/btrfs" device remove "$REPLACE1" "$TEST_MNT"
run_check $SUDO_HELPER "$TOP/btrfs" device remove "$REPLACE2" "$TEST_MNT"

for i in `seq 16`; do
	run_check $SUDO_HELPER dd if=/dev/zero of="$TEST_MNT/file$i" bs=1M count=128 status=noxfer
done
# Sync so replace start does not block in unwritten IO
run_check "$TOP/btrfs" filesystem sync "$TEST_MNT"
run_check "$TOP/btrfs" filesystem usage -T "$TEST_MNT"

# Go background, should not be that fast.
run_check $SUDO_HELPER "$TOP/btrfs" replace start 2 "$REPLACE1" "$TEST_MNT"
# No background, should wait
run_check $SUDO_HELPER "$TOP/btrfs" replace start --enqueue 3 "$REPLACE2" "$TEST_MNT"
run_check $SUDO_HELPER "$TOP/btrfs" replace status "$TEST_MNT"

run_check "$TOP/btrfs" filesystem sync "$TEST_MNT"
run_check "$TOP/btrfs" filesystem usage -T "$TEST_MNT"
run_check $SUDO_HELPER "$TOP/btrfs" replace status "$TEST_MNT"

run_check_umount_test_dev

cleanup_loopdevs
