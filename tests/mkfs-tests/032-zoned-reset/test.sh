#!/bin/bash
# Verify mkfs for zoned devices support block-group-tree feature

source "$TEST_TOP/common" || exit

check_global_prereq blkzone
setup_root_helper
# Create one 128M device with 4M zones, 32 of them
setup_nullbdevs 1 128 4

prepare_nullbdevs

TEST_DEV="${nullb_devs[1]}"
last_zone_sector=$(( 4 * 31 * 1024 * 1024 / 512 ))
# Write some data to the last zone
run_check $SUDO_HELPER dd if=/dev/urandom of="${TEST_DEV}" bs=1M count=4 seek=$(( 4 * 31 ))
# Use single as it's supported on more kernels
run_check $SUDO_HELPER "$TOP/mkfs.btrfs" -f -m single -d single "${TEST_DEV}"
# Check if the lat zone is empty
run_check_stdout $SUDO_HELPER blkzone report -o ${last_zone_sector} -c 1 "${TEST_DEV}" | grep -Fq '(em)'
if [ $? != 0 ]; then
	_fail "last zone is not empty"
fi

# Write some data to the last zone
run_check $SUDO_HELPER dd if=/dev/urandom of="${TEST_DEV}" bs=1M count=1 seek=$(( 4 * 31 ))
# Create a FS excluding the last zone
run_mayfail $SUDO_HELPER "$TOP/mkfs.btrfs" -f -b $(( 4 * 31 ))M -m single -d single "${TEST_DEV}"
if [ $? == 0 ]; then
	_fail "mkfs.btrfs should detect active zone outside of FS range"
fi

# Fill the last zone to finish it
run_check $SUDO_HELPER dd if=/dev/urandom of="${TEST_DEV}" bs=1M count=3 seek=$(( 4 * 31 + 1 ))
# Create a FS excluding the last zone
run_mayfail $SUDO_HELPER "$TOP/mkfs.btrfs" -f -b $(( 4 * 31 ))M -m single -d single "${TEST_DEV}"
# Check if the lat zone is not empty
run_check_stdout $SUDO_HELPER blkzone report -o ${last_zone_sector} -c 1 "${TEST_DEV}" | grep -Fq '(em)'
if [ $? == 0 ]; then
	_fail "last zone is empty"
fi

cleanup_nullbdevs
