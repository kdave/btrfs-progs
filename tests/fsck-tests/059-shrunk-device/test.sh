#!/bin/bash
#
# Make sure "btrfs check" can detect device smaller than its total_bytes
# in device item
#

source "$TEST_TOP/common"

setup_root_helper

file="img"
# Allocate an initial 1G file for testing.
_mktemp_local "$file" 1g

dev=$(run_check_stdout $SUDO_HELPER losetup --find --show "$file")

run_check "$TOP/mkfs.btrfs" -f "$dev"

# The original device size from prepare_loopdevs is 2G.
# Since the fs is empty, shrinking it to 996m will not cause any
# lose of metadata.
run_check $SUDO_HELPER losetup -d "$dev"
truncate -s 996m "$file"
dev=$(run_check_stdout $SUDO_HELPER losetup --find --show "$file")

run_mustfail "btrfs check should detect errors in device size" \
	"$TOP/btrfs" check "$dev"

losetup -d "$dev"
rm -- "$file"
