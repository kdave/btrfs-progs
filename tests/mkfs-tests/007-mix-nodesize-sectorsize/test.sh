#!/bin/bash
# iterate over nodesize and sectorsize combinations

source "$TEST_TOP/common"

check_prereq mkfs.btrfs
check_prereq btrfs

setup_root_helper
prepare_test_dev

test_mkfs_single()
{
	run_check_mkfs_test_dev "$@"
	run_check $SUDO_HELPER "$TOP/btrfs" inspect-internal dump-super "$TEST_DEV"
	run_check $SUDO_HELPER "$TOP/btrfs" check "$TEST_DEV"
}

# default
test_mkfs_single

# nodesize >= sectorsize
for nodesize in 4096 8192 16384 32768 65536; do
	for sectorsize in 4096 8192 16384 32768 65536; do
		[ "$nodesize" -lt "$sectorsize" ] && continue
		test_mkfs_single -n "$nodesize" -s "$sectorsize" -d single -m single
		test_mkfs_single -n "$nodesize" -s "$sectorsize" -d single -m dup
	done
done

# nodesize, mixed mode
for nodesize in 4k 8k 16k 32k 64k; do
	test_mkfs_single -n "$nodesize" -s "$nodesize" -d single -m single --mixed
	test_mkfs_single -n "$nodesize" -s "$nodesize" -d dup    -m dup    --mixed
done
