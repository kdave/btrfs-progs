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
limit=$((128*1024*1024))

# Create dataset of approximate size 256M so the repeated compression does not take that long
for file in /usr/bin/[gx]*; do
	run_check cp -axf --update --no-preserve=ownership,context "$file" "$tmp"
	size=$(du -sb "$tmp" | awk '{print $1}')
	if [ "$size" -gt "$limit" ]; then
		break
	fi
done

for file in /usr/lib*/lib[abcdef]*; do
	run_check cp -axf --update --no-preserve=ownership,context "$file" "$tmp"
	size=$(du -sb "$tmp" | awk '{print $1}')
	if [ "$size" -gt "$limit" ]; then
		break
	fi
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
