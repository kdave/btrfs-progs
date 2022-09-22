#!/bin/bash
# Test parallel reset of zones.
# Needs kernel modules: null_blk, configfs and at least 2G of free memory for
# the devices

source "$TEST_TOP/common"

setup_root_helper
prepare_test_dev

if ! check_min_kernel_version 5.12; then
	_not_run "zoned tests need kernel 5.12 and newer"
fi

nullb="$TEST_TOP/nullb"
# Create 128M devices with 4M zones, 32 of them
size=128
zone=4
# More than 10 may fail
count=10

declare -a devices
declare -a names

run_check $SUDO_HELPER "$nullb" setup
if [ $? != 0 ]; then
	_not_run "cannot setup nullb environment for zoned devices"
fi

# Record any other pre-existing devices in case creation fails
run_check $SUDO_HELPER "$nullb" ls

for i in `seq $count`; do
	# Last line has the name of the device node path
	out=$(run_check_stdout $SUDO_HELPER "$nullb" create -s "$size" -z "$zone")
	if [ $? != 0 ]; then
		_fail "cannot create nullb zoned device $i"
	fi
	devices[$i]=$(echo "$out" | tail -n 1)
	names[$i]=$(basename "${devices[$i]}")
done

run_check $SUDO_HELPER "$nullb" ls

TEST_DEV="${devices[1]}"
# Use single as it's supported on more kernels
run_check $SUDO_HELPER "$TOP/mkfs.btrfs" -d single -m single "${devices[@]}"
run_check_mount_test_dev
run_check $SUDO_HELPER dd if=/dev/zero of="$TEST_MNT"/file bs=1M count=1
run_check $SUDO_HELPER "$TOP/btrfs" filesystem usage -T "$TEST_MNT"
run_check_umount_test_dev

for i in `seq $count`; do
	run_check $SUDO_HELPER "$nullb" rm "${names[$i]}"
done
