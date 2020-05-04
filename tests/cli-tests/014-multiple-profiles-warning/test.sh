#!/bin/bash
# Create filesystem with multiple block groups and check commands that are
# supposed to warn about that:
# - device add
# - device delete
# - device usage
# - balance pause
# - balance cancel
# - filesystem df
#
# Tested for separate data/metadata and mixed

source "$TEST_TOP/common"

check_prereq mkfs.btrfs
check_prereq btrfs

setup_root_helper

test_run_commands() {
	run_check "$TOP/btrfs" filesystem usage "$TEST_MNT"
	# Report: filesystem df
	if ! run_check_stdout "$TOP/btrfs" filesystem df "$TEST_MNT" | \
		grep -q "$msg"; then
		_fail "filesystem df does not warn"
	fi
	# Report: device delete
	if ! run_check_stdout $SUDO_HELPER "$TOP/btrfs" device delete "${loopdevs[4]}" "$TEST_MNT" | \
		grep -q "$msg"; then
		_fail "device delete does not warn"
	fi
	# Report: device add
	if ! run_check_stdout $SUDO_HELPER "$TOP/btrfs" device add -f "${loopdevs[4]}" "$TEST_MNT" | \
		grep -q "$msg"; then
		_fail "device add does not warn"
	fi
	# Report: device usage
	if ! run_check_stdout $SUDO_HELPER "$TOP/btrfs" device usage "$TEST_MNT" | \
		grep -q "$msg"; then
		_fail "device usage does not warn"
	fi
	# Balance status
	out=$(run_mayfail_stdout $SUDO_HELPER "$TOP/btrfs" balance pause "$TEST_MNT")
	_log "$out"
	if ! echo "$out" | grep -q "$msg"; then
		_fail "balance pause does not warn"
	fi

	out=$(run_mayfail_stdout $SUDO_HELPER "$TOP/btrfs" balance cancel "$TEST_MNT")
	_log "$out"
	if ! echo "$out" | grep -q "$msg"; then
		_fail "balance cancel does not warn"
	fi
}

setup_loopdevs 4
prepare_loopdevs
dev1=${loopdevs[1]}
TEST_DEV=$dev1
msg="Multiple block group profiles detected"

# Data and metadata
run_check $SUDO_HELPER "$TOP/mkfs.btrfs" -f -d single -m single "${loopdevs[@]}"
run_check_mount_test_dev
run_check "$TOP/btrfs" filesystem usage "$TEST_MNT"
for i in `seq 10`; do
	run_check $SUDO_HELPER dd if=/dev/zero of="$TEST_MNT"/file$i bs=100M count=1 status=none
done
# Create filesystem with single and RAID1 profiles
run_check $SUDO_HELPER "$TOP/btrfs" balance start -dconvert=raid1,limit=1 "$TEST_MNT"

test_run_commands
run_check_umount_test_dev

# The same, with mixed profiles
run_check $SUDO_HELPER "$TOP/mkfs.btrfs" -f --mixed -d single -m single "${loopdevs[@]}"
run_check_mount_test_dev
run_check "$TOP/btrfs" filesystem usage "$TEST_MNT"
# Create 1 and a half of 1G chunks
for i in `seq 14`; do
	run_check $SUDO_HELPER dd if=/dev/zero of="$TEST_MNT"/file$i bs=100M count=1 status=none
done
# Create filesystem with single and RAID1 profiles, the limit=1 trick does not work
# so use the usage filter to convert about half of the filesystem
run_check $SUDO_HELPER "$TOP/btrfs" balance start -mconvert=raid1,usage=50 -dconvert=raid1,usage=50 "$TEST_MNT"

test_run_commands
run_check_umount_test_dev

cleanup_loopdevs
