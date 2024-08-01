#!/bin/bash
#
# Test if "mkfs.btrfs --rootdir" would handle hard links where one is inside
# the rootdir, the other out of the rootdir.

source "$TEST_TOP/common" || exit

prepare_test_dev

tmpdir=$(_mktemp_dir mkfs-rootdir-hardlinks)

run_check mkdir "$tmpdir/rootdir"
run_check touch "$tmpdir/rootdir/inside_link"
run_check ln "$tmpdir/rootdir/inside_link" "$tmpdir/outside_link"

run_check_mkfs_test_dev --rootdir "$tmpdir/rootdir"

# For older mkfs.btrfs --rootdir we will create inside_link with 2 links,
# but since the other one is out of the rootdir, there should only be one
# 1 link, leading to btrfs check fail.
#
# The new behavior will split all hard links into different inodes, thus
# have correct nlink for each new inode.
run_check $SUDO_HELPER "$TOP/btrfs" check "$TEST_DEV"
