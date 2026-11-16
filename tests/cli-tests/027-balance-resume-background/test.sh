#!/bin/bash
#
# Resume a paused balance with 'balance resume --background'

source "$TEST_TOP/common" || exit

check_prereq mkfs.btrfs
check_prereq btrfs
check_global_prereq dd
check_global_prereq timeout

setup_root_helper
prepare_test_dev

run_check_mkfs_test_dev
run_check_mount_test_dev

# Same as fstests btrfs/213: write for a fixed time, up to half of the device,
# so that balancing the data takes a while on fast machines too.
start=$(date +%s)
run_mayfail $SUDO_HELPER timeout 8 dd if=/dev/zero of="$TEST_MNT/file" \
	bs=1M count=1024 oflag=direct status=none
ret=$?
runtime=$(( $(date +%s) - start ))
# 124 is the timeout stopping dd, 0 is dd writing all of it before that.
# Anything else is a failed write, e.g. a full device, so don't go on.
if [ "$ret" != 0 ] && [ "$ret" != 124 ]; then
	run_check_umount_test_dev
	_not_run "timed write failed (ret=$ret)"
fi

# Read the data back from the device during balance
run_check_umount_test_dev
run_check_mount_test_dev

run_check $SUDO_HELPER "$TOP/btrfs" balance start -d --bg "$TEST_MNT"
sleep $(( runtime / 4 ))
if ! run_mayfail $SUDO_HELPER "$TOP/btrfs" balance pause "$TEST_MNT"; then
	run_check_umount_test_dev
	_not_run "balance finished before it could be paused"
fi

run_check $SUDO_HELPER "$TOP/btrfs" balance resume --bg "$TEST_MNT"

# A paused balance does not go on by itself, so wait until it is not paused
# anymore
resumed=false
for i in $(seq 60); do
	if ! run_mayfail_stdout $SUDO_HELPER "$TOP/btrfs" balance status "$TEST_MNT" |
	     grep -F "is paused" >/dev/null; then
		resumed=true
		break
	fi
	sleep 1
done
if ! $resumed; then
	_fail "balance not resumed by resume --background"
fi

run_mayfail $SUDO_HELPER "$TOP/btrfs" balance cancel "$TEST_MNT"
run_check_umount_test_dev
