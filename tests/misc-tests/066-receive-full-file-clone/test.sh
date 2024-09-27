#!/bin/bash
# Verify "btrfs-receive" can handle a full file clone (clone length is not
# aligned but matches inode size) into a larger destination file.
#
# Such clone stream can be generated since kernel commit 46a6e10a1ab1
# ("btrfs: send: allow cloning non-aligned extent if it ends at i_size").

source "$TEST_TOP/common" || exit
source "$TEST_TOP/common.convert" || exit

tmp=$(_mktemp "receive-full-file-clone")

setup_root_helper
prepare_test_dev
check_global_prereq zstd

run_check_mkfs_test_dev
run_check_mount_test_dev
run_check zstd -d -f ./ro_subv1.stream.zst -o "$tmp"
run_check "$TOP/btrfs" receive -f "$tmp" "$TEST_MNT"
run_check zstd -d -f ./ro_snap1.stream.zst -o "$tmp"
run_check "$TOP/btrfs" receive -f "$tmp" "$TEST_MNT"
run_check_umount_test_dev
rm -f -- "$tmp"
