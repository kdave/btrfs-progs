#!/bin/bash
# Ensure that running btrfs check on a fs which has name collisions of files
# doesn't result in false positives. This test is specifically targeted at
# lowmem mode.

source "$TEST_TOP/common"

check_prereq btrfs
check_prereq mkfs.btrfs

setup_root_helper
prepare_test_dev

run_check_mkfs_test_dev
run_check_mount_test_dev

# Create 2 files whose names collide
run_check $SUDO_HELPER touch "$TEST_MNT/5ab4e206~~~~~~~~XVT1U3ZF647YS2PD4AKAG826"
run_check $SUDO_HELPER touch "$TEST_MNT/5ab4e26a~~~~~~~~AP1C3VQBE79IJOTVOEZIR9YU"

run_check_umount_test_dev

# The fs is clean so lowmem shouldn't produce any warnings
run_check "$TOP/btrfs" check --readonly "$TEST_DEV"
