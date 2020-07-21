#!/bin/bash
# Regression test for mkfs.btrfs --rootdir with DUP data profile and rootdir
# size near the limit of the device.
#
# There is a bug that makes mkfs.btrfs always to create unnecessary SINGLE
# chunks, which eats up a lot of space and leads to unexpected ENOSPC bugs.

source "$TEST_TOP/common"

check_prereq mkfs.btrfs
prepare_test_dev

tmp=$(mktemp -d --tmpdir btrfs-progs-mkfs.rootdirXXXXXXX)

fallocate -l 128M $tmp/large_file

# We should be able to create the fs with size limit to 2 * (128 + 32 + 8)
# which is 336M. Here we round it up to 350M.
run_check "$TOP/mkfs.btrfs" -f --rootdir "$tmp" -d dup -b 350M "$TEST_DEV"
run_check "$TOP/btrfs" check "$TEST_DEV"

rm -rf -- "$tmp"
