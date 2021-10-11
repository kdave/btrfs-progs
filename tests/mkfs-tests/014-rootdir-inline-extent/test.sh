#!/bin/bash
# Regression test for mkfs.btrfs --rootdir with inline file extents
# For any large inline file extent, btrfs check could already report it

source "$TEST_TOP/common"

check_prereq mkfs.btrfs

prepare_test_dev

tmp=$(_mktemp_dir mkfs-rootdir)

pagesize=$(getconf PAGESIZE)
create_file()
{
	local size=$1
	# Reuse size as filename
	eval printf "%0.sx" {1..$size} > "$tmp/$size"
}

test_mkfs_rootdir()
{
	nodesize=$1
	run_check_mkfs_test_dev --nodesize "$nodesize" --rootdir "$tmp"
	run_check $SUDO_HELPER "$TOP/btrfs" check "$TEST_DEV"
}

# Use power of 2 from 512 to 64K (maximum node size) as base file size
for i in 512 1024 2048 4096 8192 16384 32768; do
	create_file $(($i - 102))
	# 101 is the overhead size for max inline extent
	create_file $(($i - 101))
	create_file $(($i - 100))

	create_file $(($i - 1))
	create_file $i
	create_file $(($i + 1))
done

for nodesize in 4096 8192 16384 32768 65536; do
	if [ "$nodesize" -ge "$pagesize" ]; then
		test_mkfs_rootdir "$nodesize"
	fi
done
rm -rf -- "$tmp"
