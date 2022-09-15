#!/bin/bash
#
# minimal test for the following syntax: btrfs send -p parent subvol1 subvol2

source "$TEST_TOP/common"

check_prereq mkfs.btrfs
check_prereq btrfs

setup_root_helper
prepare_test_dev

run_check_mkfs_test_dev
run_check_mount_test_dev

here=`pwd`
cd "$TEST_MNT" || _fail "cannot chdir to TEST_MNT"

run_check $SUDO_HELPER "$TOP/btrfs" subvolume create subv-parent
run_check $SUDO_HELPER dd if=/dev/urandom of=subv-parent/file bs=1M count=10
run_check $SUDO_HELPER "$TOP/btrfs" subvolume snapshot -r subv-parent subv-snap1
run_check $SUDO_HELPER dd if=/dev/urandom of=subv-parent/file bs=1M count=10
run_check $SUDO_HELPER "$TOP/btrfs" subvolume snapshot -r subv-parent subv-snap2
run_check $SUDO_HELPER dd if=/dev/urandom of=subv-parent/file bs=1M count=10
run_check $SUDO_HELPER "$TOP/btrfs" subvolume snapshot -r subv-parent subv-snap3

stream=$(_mktemp)
run_check $SUDO_HELPER "$TOP/btrfs" send -f "$stream" -p subv-snap1 subv-snap2 subv-snap3

cd "$here" || _fail "cannot chdir back to test directory"
rm "$stream"

run_check_umount_test_dev
