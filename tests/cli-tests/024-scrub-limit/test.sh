#!/bin/bash
# Read and set scrub limits on a filesystem

source "$TEST_TOP/common" || exit

setup_root_helper
setup_loopdevs 4
prepare_loopdevs
TEST_DEV=${loopdevs[1]}
support=true

fsid="13411a59-ccea-4296-a6f8-1446ccf8c9be"
sysfs="/sys/fs/btrfs/13411a59-ccea-4296-a6f8-1446ccf8c9be"

run_check $SUDO_HELPER "$TOP/mkfs.btrfs" -f --uuid "$fsid" -d raid1 -m raid1 "${loopdevs[@]}"
run_check_mount_test_dev

# Set the limits directly
for i in "$sysfs"/devinfo/*/scrub_speed_max; do
	if ! [ -f "$i" ]; then
		_log "sysfs file scrub_speed_max not available, skip setting limits"
		support=false
		break;
	fi
	run_check cat "$i"
	echo "10m" | run_check $SUDO_HELPER tee "$i" >/dev/null
done
# This works even if scrub_speed_max files don't exist, this is equivalent to unlimited
run_check "$TOP/btrfs" scrub limit "$TEST_MNT"

# The rest of the test would fail
if ! $support; then
	run_check_umount_test_dev
	cleanup_loopdevs
fi

# Set the limits by command
here=`pwd`
cd "$sysfs/devinfo" || _fail "Cannot cd to $sysfs/devinfo"
for i in *; do
	run_check $SUDO_HELPER "$TOP/btrfs" scrub limit -d "$i" -l 20m "$TEST_MNT"
done
cd "$here" || _fail "Cannot cd to $here"
run_check "$TOP/btrfs" scrub limit "$TEST_MNT"

# Set limits for all devices
run_check $SUDO_HELPER "$TOP/btrfs" scrub limit -a -l 30m "$TEST_MNT"
run_check "$TOP/btrfs" scrub limit "$TEST_MNT"

run_check_umount_test_dev

cleanup_loopdevs
