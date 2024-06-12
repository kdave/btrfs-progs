#!/bin/bash
# Verify btrfstune for zoned devices with block-group-tree conversion

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
name=$(basename "$dev")

run_check $SUDO_HELPER "$nullb" ls

TEST_DEV="$dev"

# Create the fs without bgt
run_check $SUDO_HELPER "$TOP/mkfs.btrfs" -f -m single -d single -O ^block-group-tree "$dev"
run_check_mount_test_dev
run_check $SUDO_HELPER dd if=/dev/zero of="$TEST_MNT"/file1 bs=1M count=1
run_check $SUDO_HELPER "$TOP/btrfs" filesystem usage -T "$TEST_MNT"
run_check_umount_test_dev

# Convert to bgt
run_check $SUDO_HELPER "$TOP/btrfstune" --convert-to-block-group-tree "$dev"
run_check_mount_test_dev
run_check $SUDO_HELPER dd if=/dev/zero of="$TEST_MNT"/file2 bs=1M count=1
run_check $SUDO_HELPER "$TOP/btrfs" filesystem usage -T "$TEST_MNT"
run_check_umount_test_dev

# And convert back to old extent tree
run_check $SUDO_HELPER "$TOP/btrfstune" --convert-from-block-group-tree "$dev"
run_check_mount_test_dev
run_check $SUDO_HELPER dd if=/dev/zero of="$TEST_MNT"/file3 bs=1M count=1
run_check $SUDO_HELPER "$TOP/btrfs" filesystem usage -T "$TEST_MNT"
run_check_umount_test_dev

run_check $SUDO_HELPER "$nullb" rm "$name"
