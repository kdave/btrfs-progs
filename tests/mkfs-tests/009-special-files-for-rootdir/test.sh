#!/bin/bash
# Check if --rootdir can handle special files (socket/fifo/char/block) correctly
#
# --rootdir had a problem of filling dir items/indexes with wrong type
# and caused btrfs check to report such error
#
# Note: sock type is skipped in this test

source "$TEST_TOP/common"

check_prereq mkfs.btrfs
check_prereq btrfs

setup_root_helper		# For mknod
prepare_test_dev

# mknod can create FIFO/CHAR/BLOCK file but not SOCK.
# No neat tool to create socket file, unless using python or similar.
# So no SOCK is tested here
check_global_prereq mknod
check_global_prereq dd

tmp=$(_mktemp_dir mkfs-rootdir)

run_check mkdir "$tmp/dir"
run_check mkdir -p "$tmp/dir/in/dir"
run_check mknod "$tmp/fifo" p
run_check $SUDO_HELPER mknod "$tmp/char" c 1 1
run_check $SUDO_HELPER mknod "$tmp/block" b 1 1
run_check dd if=/dev/zero bs=1M count=1 of="$tmp/regular"

run_check_mkfs_test_dev -r "$tmp"

rm -rf -- "$tmp"

run_check $SUDO_HELPER "$TOP/btrfs" check "$TEST_DEV"
