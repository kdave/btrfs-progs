#!/bin/bash
# Verify that a seeding device can be mounted several times

source "$TEST_TOP/common"

check_prereq btrfs-image
check_prereq mkfs.btrfs
check_prereq btrfs

setup_root_helper

setup_loopdevs 6
prepare_loopdevs

ORIG_TEST_DEV="$TEST_DEV"
ORIG_TEST_MNT="$TEST_MNT"

# Create seeding device
TEST_DEV=${loopdevs[1]}
seeddev=${loopdevs[1]}
run_check_mkfs_test_dev -L BTRFS-TESTS-SEED
run_check_mount_test_dev

for i in `seq 6`; do
	run_check $SUDO_HELPER dd if=/dev/zero of="$TEST_MNT/file${i}" bs=1M count=1 status=none
	# Something to distinguish the contents
	run_check md5sum "$TEST_MNT/file${i}"
done
run_check_umount_test_dev

# Mark 1 as seeding
run_check $SUDO_HELPER "$TOP/btrfstune" -S 1 "$TEST_DEV"
TEST_DEV=${loopdevs[1]}

nextdevice() {
	local nextdev

	nextdev="$1"
	# Mount again, as seeding device
	TEST_DEV=${loopdevs[1]}
	mnt=mnt$nextdev
	run_check mkdir -p "$mnt"
	TEST_MNT="$mnt"
	run_check_mount_test_dev
	run_check $SUDO_HELPER "$TOP/btrfs" device add ${loopdevs[$nextdev]} "$TEST_MNT"
	run_mustfail "writable file despite read-only mount" \
		$SUDO_HELPER dd if=/dev/zero of="$TEST_MNT/file$nextdevice" bs=1M count=1 status=none
	run_check $SUDO_HELPER mount -o remount,rw "$TEST_MNT"
	# Rewrite the file
	md5sum=$(run_check_stdout md5sum "$TEST_MNT/file$nextdev" | awk '{print $1}')
	yes "$nextdev" | run_check $SUDO_HELPER dd of="$TEST_MNT/file$nextdev" bs=1M count=1 status=none
	md5sum2=$(run_check_stdout md5sum "$TEST_MNT/file$nextdev" | awk '{print $1}')
	if [ "$md5sum" == "$md5sum2" ]; then
		_fail "file contents mismatch after rewrite"
	fi
	# Umount by the new device
	run_check_umount_test_dev "${loopdevs[$nextdev]}"

	# Try to mount it again by the new device
	TEST_DEV="${loopdevs[$nextdev]}"
	run_check_mount_test_dev
	# Check that it stays writable
	run_check $SUDO_HELPER touch "$TEST_MNT"/writable-after-remount
	# Change label
	run_check $SUDO_HELPER "$TOP/btrfs" filesystem label "$TEST_MNT" "BTRFS-TESTS-SEED-$nextdev"
	# And that the contents is the same
	md5sum=$(run_check_stdout md5sum "$TEST_MNT/file$nextdev" | awk '{print $1}')
	if [ "$md5sum" != "$md5sum2" ]; then
		_fail "file contents not same after remount"
	fi
}

# Keep previous device(s) mounted, create a new filesystem from the seeding device
nextdevice 2
nextdevice 3
nextdevice 4
nextdevice 5

# Final umount
# Skip seeding device, loop device 1,
run_check $SUDO_HELPER umount ${loopdevs[2]}
run_check $SUDO_HELPER umount ${loopdevs[3]}
run_check $SUDO_HELPER umount ${loopdevs[4]}
run_check $SUDO_HELPER umount ${loopdevs[5]}

cleanup_loopdevs

rm -rf -- mnt[0-9]
