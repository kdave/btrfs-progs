#!/bin/bash
# Test btrfs-image with multiple devices filesystem and verify that restoring
# the created image works against a single device.

source "$TEST_TOP/common"

check_prereq btrfs-image
check_prereq mkfs.btrfs
check_prereq btrfs

setup_root_helper

rm -f dev1 dev2
run_check truncate -s 2G dev1
run_check truncate -s 2G dev2
chmod a+w dev1 dev2

loop1=$(run_check_stdout $SUDO_HELPER losetup --find --show dev1)
loop2=$(run_check_stdout $SUDO_HELPER losetup --find --show dev2)

# Create the test file system.

run_check $SUDO_HELPER "$TOP/mkfs.btrfs" -f "$loop1" "$loop2"
run_check $SUDO_HELPER mount "$loop1" "$TEST_MNT"
run_check $SUDO_HELPER dd bs=1M count=1 if=/dev/zero of="$TEST_MNT/foobar"
orig_md5=$(run_check_stdout md5sum "$TEST_MNT/foobar" | cut -d ' ' -f 1)
run_check $SUDO_HELPER umount "$TEST_MNT"

# Create the image to restore later.
run_check $SUDO_HELPER "$TOP/btrfs-image" "$loop1" "$IMAGE"

# Wipe out the filesystem from the devices, restore the image on a single
# device, check everything works and file foobar is there and with 1Mb of
# zeroes.
run_check $SUDO_HELPER wipefs -a "$loop1"
run_check $SUDO_HELPER wipefs -a "$loop2"

run_check $SUDO_HELPER "$TOP/btrfs-image" -r "$IMAGE" "$loop1"

# Run check to make sure there is nothing wrong for the recovered image
run_check "$TOP/btrfs" check "$loop1"

run_check $SUDO_HELPER mount "$loop1" "$TEST_MNT"
new_md5=$(run_check_stdout md5sum "$TEST_MNT/foobar" | cut -d ' ' -f 1)
run_check $SUDO_HELPER umount "$TEST_MNT"

# Cleanup loop devices.
run_check $SUDO_HELPER losetup -d "$loop1"
run_check $SUDO_HELPER losetup -d "$loop2"
rm -f dev1 dev2

# Compare the file digests.
[ "$orig_md5" == "$new_md5" ] || _fail "File digests do not match"
