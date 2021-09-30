#!/bin/bash
# confirm that clearing space cache works

source "$TEST_TOP/common"

check_prereq btrfs
check_prereq mkfs.btrfs

setup_root_helper
prepare_test_dev

run_check_mkfs_test_dev
run_check_mount_test_dev

# Create files that takes at least 3 data chunks, while
# can still be removed to create free space inside one chunk.

for i in $(seq 0 6); do
	run_check $SUDO_HELPER dd if=/dev/zero of="$TEST_MNT/file_${i}" bs=1M \
		count=64 > /dev/null 2>&1
done
sync

# Remove file 1 3 5 to create holes
for i in 1 3 5; do
	run_check $SUDO_HELPER rm "$TEST_MNT/file_${i}"
done

sync

run_check_umount_test_dev

# Clear space cache and re-check fs
run_check "$TOP/btrfs" check --clear-space-cache v1 "$TEST_DEV"
run_check "$TOP/btrfs" check "$TEST_DEV"

# Manually recheck space cache and super space cache generation
run_check_stdout "$TOP/btrfs" inspect-internal dump-tree -t root "$TEST_DEV" | \
	grep -q -w FREE_SPACE
if [ $? -eq 0 ]; then
	_fail "clear space cache doesn't clear all space cache"
fi

run_check_stdout $TOP/btrfs inspect-internal dump-super "$TEST_DEV" |
	grep -q 'cache_generation.*18446744073709551615'
if [ $? -ne 0 ]; then
	_fail "clear space cache doesn't set cache_generation correctly"
fi
