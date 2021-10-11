#!/bin/bash
# Check if mkfs runtime feature quota can handle --rootdir

source "$TEST_TOP/common"

check_prereq mkfs.btrfs
check_prereq btrfs

setup_root_helper
prepare_test_dev

# mknod can create FIFO/CHAR/BLOCK file but not SOCK.
# No neat tool to create socket file, unless using python or similar.
# So no SOCK is tested here
check_global_prereq mknod
check_global_prereq dd

tmp=$(_mktemp_dir mkfs-rootdir)

run_check mkdir "$tmp/dir"
run_check mkdir -p "$tmp/dir/in/dir"

# More dir, there is no good way to pump metadata since we have no trigger
# to enable/disable inline extent data, so here create enough dirs to bump
# metadata
run_check mkdir "$tmp/a_lot_of_dirs"
for i in $(seq -w 0 8192); do
	run_check mkdir "$tmp/a_lot_of_dirs/dir_$i"
done

# Then some data
run_check dd if=/dev/zero bs=1M count=1 of="$tmp/1M"
run_check dd if=/dev/zero bs=2M count=1 of="$tmp/2M"
run_check dd if=/dev/zero bs=4M count=1 of="$tmp/4M"
run_check dd if=/dev/zero bs=8M count=1 of="$tmp/8M"

run_check dd if=/dev/zero bs=1K count=1 of="$tmp/1K"
run_check dd if=/dev/zero bs=2K count=1 of="$tmp/2K"
run_check dd if=/dev/zero bs=4K count=1 of="$tmp/4K"
run_check dd if=/dev/zero bs=8K count=1 of="$tmp/8K"

run_check $SUDO_HELPER "$TOP/mkfs.btrfs" -f --rootdir "$tmp" -R quota "$TEST_DEV"

rm -rf -- "$tmp"

# Normal check already includes quota check
run_check $SUDO_HELPER "$TOP/btrfs" check "$TEST_DEV"
run_check $SUDO_HELPER "$TOP/btrfs" check --qgroup-report "$TEST_DEV"
