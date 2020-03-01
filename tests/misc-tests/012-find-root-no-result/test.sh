#!/bin/bash
# Regression test for case btrfs-find-root may print no result on a
# recent fs or balanced fs, whose metadata chunk is the first chunk
# and the only metadata chunk

source "$TEST_TOP/common"

check_prereq btrfs-find-root
check_prereq btrfs-image

run_check "$TOP/btrfs-image" -r first_meta_chunk.btrfs-image test.img || \
	_fail "failed to extract first_meta_chunk.btrfs-image"

result=$(run_check_stdout "$INTERNAL_BIN/btrfs-find-root" test.img | sed '/^Superblock/d')

if [ -z "$result" ]; then
	_fail "btrfs-find-root failed to find tree root"
fi

if ! echo "$result" | grep -q 'Found tree root at'; then
	_fail "btrfs-find-root failed to find tree root, unexpected output"
fi

rm test.img
