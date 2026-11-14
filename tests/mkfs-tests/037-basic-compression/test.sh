#!/bin/bash
# Basic test for mkfs.btrfs --compress --rootdir. Create a dataset and use it
# for filesystem creation with various compression levels.

source "$TEST_TOP/common" || exit

check_prereq mkfs.btrfs
check_prereq btrfs
check_global_prereq du

setup_root_helper
prepare_test_dev

tmp=$(_mktemp_dir mkfs-rootdir)

# Create dataset of approximate size 256M so the repeated compression does not
# take that long.
# Mix compressible, incompressible and partially compressible files.
for i in $(seq 1 25); do
	# Compressible: zeros
	run_check dd if=/dev/zero of="$tmp/zero_$i" bs=1M count=4 status=none
	# Incompressible: random data
	run_check dd if=/dev/urandom of="$tmp/random_$i" bs=1M count=4 status=none
	# Partially compressible: 64K of random data alternating with 64K of
	# zeros, so a compressed extent (max 128K) contains both
	run_check dd if=/dev/zero of="$tmp/mixed_$i" bs=1M count=2 status=none
	for j in $(seq 0 15); do
		run_check dd if=/dev/urandom of="$tmp/mixed_$i" bs=64K count=1 \
			seek=$((j * 2)) conv=notrunc status=none
	done
done

run_check du -sh "$tmp"

run_test()
{
	local comp="$1"

	run_check_mkfs_test_dev --rootdir "$tmp" --compress "$comp"
	run_check $SUDO_HELPER "$TOP/btrfs" check "$TEST_DEV"

	run_check_mount_test_dev
	run_check du -sh "$TEST_MNT"
	run_check_umount_test_dev
}

run_test lzo
run_test zlib
run_test zlib:1
run_test zlib:9
run_test zstd
run_test zstd:1
run_test zstd:15

run_check rm -rf -- "$tmp"
