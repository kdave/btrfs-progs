#!/bin/bash
# Test that btrfs-image can dump image correctly for a missing device (RAID1)
#
# At least for RAID1, btrfs-image should be able to handle one missing device
# without any problem

source "$TEST_TOP/common"

check_prereq btrfs-image
check_prereq mkfs.btrfs
check_prereq btrfs

setup_root_helper
setup_loopdevs 2
prepare_loopdevs
dev1=${loopdevs[1]}
dev2=${loopdevs[2]}

# $1:	device number to remove (either 1 or 2)
test_missing()
{
	local bad_num
	local bad_dev
	local good_num
	local good_dev

	bad_num=$1
	bad_dev=${loopdevs[$bad_num]}
	good_num=$((3 - $bad_num))
	good_dev=${loopdevs[$good_num]}

	run_check $SUDO_HELPER "$TOP/mkfs.btrfs" -f -d raid1 -m raid1 "$dev1" "$dev2"

	# fill the fs with some data, we could create space cache
	run_check $SUDO_HELPER mount "$dev1" "$TEST_MNT"
	run_check $SUDO_HELPER dd if=/dev/zero of="$TEST_MNT/a" bs=1M count=10
	run_check $SUDO_HELPER dd if=/dev/zero of="$TEST_MNT/b" bs=4k count=1000 conv=sync
	run_check $SUDO_HELPER umount "$TEST_MNT"

	# make sure we have space cache
	if ! run_check_stdout "$TOP/btrfs" inspect dump-tree -t root "$dev1" |
		grep -q "EXTENT_DATA"; then
		# Normally the above operation should create the space cache.
		# If not, it may mean we have migrated to v2 cache by default
		_not_run "unable to create v1 space cache"
	fi

	# now wipe the device
	run_check wipefs -fa "$bad_dev"

	# we don't care about the image but btrfs-image must not fail
	run_check "$TOP/btrfs-image" "$good_dev" /dev/null
}

# Test with either device missing, so we're ensured to hit missing device
test_missing 1
test_missing 2
cleanup_loopdevs
