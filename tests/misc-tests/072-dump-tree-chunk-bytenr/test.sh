#!/bin/bash
# Test "btrfs ins dump-tree -b <bytenr>" when the bytenr belongs to a chunk
# tree block.
#
# This is a regression test for commit 408e45feb325 ("btrfs-progs:
# dump-tree: add extra chunk type checks"), which is too strict and
# rejects chunk tree blocks.

source "$TEST_TOP/common" || exit
source "$TEST_TOP/common.convert" || exit

check_prereq mkfs.btrfs
check_prereq btrfs

setup_root_helper
prepare_test_dev

run_check_mkfs_test_dev
chunk_root=$(run_check_stdout "$TOP/btrfs" inspect-internal dump-tree -t chunk "$TEST_DEV" |\
	     grep "owner CHUNK_TREE" | cut -f 2 -d\ )
if [ -z "$chunk_root" ]; then
	_fail "unable to get the chunk root bytenr"
fi

run_check "$TOP/btrfs" inspect-internal dump-tree -b "$chunk_root" "$TEST_DEV"
