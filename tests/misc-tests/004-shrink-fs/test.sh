#!/bin/bash
#
# Test getting the minimum size a filesystem can be resized to and verify we
# are able to resize (shrink) it to that size.
#

source "$TEST_TOP/common"

check_prereq mkfs.btrfs
check_prereq btrfs

setup_root_helper

_get_min_dev_size()
{
	size=$(run_check_stdout $SUDO_HELPER "$TOP/btrfs" inspect-internal \
		min-dev-size ${1:+--id "$1"} "$TEST_MNT" |		   \
		grep -e "^[[:digit:]]\+.*)$" | cut -d ' ' -f 1)
	echo "$size"
}

# Optionally take id of the device to shrink
shrink_test()
{
	min_size=$(_get_min_dev_size "$1")
	_log "min size = ${min_size}"
	if [ -z "$min_size" ]; then
		_fail "Failed to parse minimum size"
	fi
	run_check $SUDO_HELPER "$TOP/btrfs" filesystem resize "$min_size" "$TEST_MNT"
}

run_check truncate -s 20G "$IMAGE"
run_check "$TOP/mkfs.btrfs" -f "$IMAGE"
run_check $SUDO_HELPER mount "$IMAGE" "$TEST_MNT"
run_check $SUDO_HELPER chmod a+rw "$TEST_MNT"

# Create 7 data block groups, each with a size of 1Gb.
for ((i = 1; i <= 7; i++)); do
	run_check fallocate -l 1G "$TEST_MNT/foo$i"
done

# Make sure they are persisted (all the chunk, device and block group items
# added to the chunk/dev/extent trees).
run_check "$TOP/btrfs" filesystem sync "$TEST_MNT"

# Now remove 3 of those 1G files. This will result in 3 block groups becoming
# unused, which will be automatically deleted by the cleaner kthread, and this
# will result in 3 holes (unallocated space) in the device (each with a size
# of 1Gb).

run_check rm -f "$TEST_MNT/foo2"
run_check rm -f "$TEST_MNT/foo4"
run_check rm -f "$TEST_MNT/foo6"

# Sync once to wake up the cleaner kthread which will delete the unused block
# groups - it could have been sleeping when they became unused. Then wait a bit
# to allow the cleaner kthread to delete them and then finally ensure the
# transaction started by the cleaner kthread is committed.
run_check "$TOP/btrfs" filesystem sync "$TEST_MNT"
sleep 3
run_check "$TOP/btrfs" filesystem sync "$TEST_MNT"

# Now attempt to get the minimum size we can resize the filesystem to and verify
# the resize operation succeeds. This size closely matches the sum of the size
# of all the allocated device extents.
for ((i = 1; i <= 3; i++)); do
	shrink_test
done

# Now convert metadata and system chunks to the single profile and check we are
# still able to get a correct minimum size and shrink to that size.
run_check $SUDO_HELPER "$TOP/btrfs" balance start -mconvert=single \
	-sconvert=single -f "$TEST_MNT"
for ((i = 1; i <= 3; i++)); do
	shrink_test 1
done

run_check $SUDO_HELPER umount "$TEST_MNT"
