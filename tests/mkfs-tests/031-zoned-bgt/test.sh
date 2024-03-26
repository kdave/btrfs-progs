#!/bin/bash
# Verify mkfs for zoned devices support block-group-tree feature

source "$TEST_TOP/common" || exit

setup_root_helper
prepare_test_dev

nullb="$TEST_TOP/nullb"
# Create one 128M device with 4M zones, 32 of them
size=128
zone=4

run_mayfail $SUDO_HELPER "$nullb" setup
if [ $? != 0 ]; then
	_not_run "cannot setup nullb environment for zoned devices"
fi

# Record any other pre-existing devices in case creation fails
run_check $SUDO_HELPER "$nullb" ls

# Last line has the name of the device node path
out=$(run_check_stdout $SUDO_HELPER "$nullb" create -s "$size" -z "$zone")
if [ $? != 0 ]; then
	_fail "cannot create nullb zoned device $i"
fi
dev=$(echo "$out" | tail -n 1)
name=$(basename "${dev}")

run_check $SUDO_HELPER "$nullb" ls

TEST_DEV="${dev}"
# Use single as it's supported on more kernels
run_check $SUDO_HELPER "$TOP/mkfs.btrfs" -m single -d single -O block-group-tree "${dev}"
run_check_mount_test_dev
run_check $SUDO_HELPER dd if=/dev/zero of="$TEST_MNT"/file bs=1M count=1
run_check $SUDO_HELPER "$TOP/btrfs" filesystem usage -T "$TEST_MNT"
run_check_umount_test_dev

run_check $SUDO_HELPER "$nullb" rm "${name}"
