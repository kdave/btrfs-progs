#!/bin/bash
# Regression test for mkfs.btrfs --rootdir on non-existent file.
# Expected behavior: it should create a new file if destination doesn't exist
# Regression 460e93f25754 ("btrfs-progs: mkfs: check the status of file at mkfs")

source "$TEST_TOP/common"

check_prereq mkfs.btrfs

tmp=$(_mktemp_dir mkfs-rootdir)
# we can't use TEST_DEV, a file is needed
img=$(_mktemp mkfs-rootdir)
run_check "$TOP/mkfs.btrfs" -f --rootdir "$INTERNAL_BIN/Documentation/" "$img"

rm -rf -- "$img"
