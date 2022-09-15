#!/bin/bash
# recognize partitioned loop devices

source "$TEST_TOP/common"

if ! losetup --help | grep -q 'partscan'; then
	_not_run "losetup --partscan not available"
	exit 0
fi

check_prereq mkfs.btrfs

setup_root_helper

_mktemp_local img
cp partition-1g-1g img
run_check truncate -s2g img

loopdev=$(run_check_stdout $SUDO_HELPER losetup --partscan --find --show img)
base=$(basename "$loopdev")

# expect partitions named like loop0p1 etc
for looppart in $(ls /dev/"$base"?*); do
	run_check $SUDO_HELPER "$TOP/mkfs.btrfs" -f "$looppart"
	run_check $SUDO_HELPER "$TOP/btrfs" inspect-internal dump-super "$looppart"
done

# cleanup
run_check $SUDO_HELPER losetup -d "$loopdev"
rm img
