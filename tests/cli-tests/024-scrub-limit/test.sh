#!/bin/bash
# Read scrub limits on a filesystem

source "$TEST_TOP/common" || exit

setup_root_helper
setup_loopdevs 4
prepare_loopdevs
TEST_DEV=${loopdevs[1]}

fsid="13411a59-ccea-4296-a6f8-1446ccf8c9be"
sysfs="/sys/fs/btrfs/13411a59-ccea-4296-a6f8-1446ccf8c9be"

run_check $SUDO_HELPER "$TOP/mkfs.btrfs" -f --uuid "$fsid" -d raid1 -m raid1 "${loopdevs[@]}"
run_check_mount_test_dev
for i in "$sysfs"/devinfo/*/scrub_speed_max; do
	if ! [ -f "$i" ]; then
		_log "sysfs file scrub_speed_max not available, skip setting limits"
		break;
	fi
	run_check cat "$i"
	echo "10m" | run_check $SUDO_HELPER tee "$i" >/dev/null
done
# This works even if scrub_speed_max files don't exist, this is equivalent to unlimited
run_check "$TOP/btrfs" scrub limit "$TEST_MNT"
run_check_umount_test_dev

cleanup_loopdevs
