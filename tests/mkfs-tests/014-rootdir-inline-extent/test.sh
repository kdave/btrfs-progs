#!/bin/bash
# Regression test for mkfs.btrfs --rootdir with inline file extents
# For any large inline file extent, btrfs check could already report it

source "$TOP/tests/common"

check_global_prereq fallocate
check_prereq mkfs.btrfs

prepare_test_dev

tmp=$(mktemp -d --tmpdir btrfs-progs-mkfs.rootdirXXXXXXX)

pagesize=$(getconf PAGESIZE)
create_file()
{
	local size=$1
	# Reuse size and filename
	run_check fallocate -l $size "$tmp/$size"
}

test_mkfs_rootdir()
{
	nodesize=$1
	run_check "$TOP/mkfs.btrfs" --nodesize $nodesize -f --rootdir "$tmp" \
		"$TEST_DEV"
	run_check $SUDO_HELPER "$TOP/btrfs" check "$TEST_DEV"
}

# File sizes is designed to cross differnet node size, so even
# the sectorsize is not 4K, we can still test it well.
create_file 512
create_file 1024
create_file 2048

create_file 3994
create_file 3995	# For 4K node size, max inline would be 4k - 101
create_file 3996

create_file 4095
create_file 4096
create_file 4097

create_file 8090
create_file 8091
create_file 8092

create_file 8191
create_file 8192
create_file 8193

create_file 16282
create_file 16283
create_file 16284

create_file 16383
create_file 16384
create_file 16385

create_file 32666
create_file 32667
create_file 32668

create_file 32767
create_file 32768
create_file 32769

create_file 65434
create_file 65435
create_file 65436

create_file 65535
create_file 65536
create_file 65537

for nodesize in 4096 8192 16384 32768 65536; do
	if [ $nodesize -ge $pagesize ]; then
		test_mkfs_rootdir $nodesize
	fi
done
rm -rf -- "$tmp"
