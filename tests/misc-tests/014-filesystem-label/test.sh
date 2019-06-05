#!/bin/bash
#
# test label settings

source "$TEST_TOP/common"

check_prereq mkfs.btrfs
check_prereq btrfs

setup_root_helper

prepare_test_dev
run_check_mkfs_test_dev -L BTRFS-TEST-LABEL
run_check_mount_test_dev
run_check $SUDO_HELPER chmod a+rw "$TEST_MNT"

cd "$TEST_MNT"
run_check $SUDO_HELPER "$TOP/btrfs" filesystem label "$TEST_MNT"
# shortest label
run_check $SUDO_HELPER "$TOP/btrfs" filesystem label "$TEST_MNT" a
run_check $SUDO_HELPER "$TOP/btrfs" filesystem label "$TEST_MNT"
run_check $SUDO_HELPER "$TOP/btrfs" filesystem label "$TEST_MNT" ''

longlabel=\
0123456789\
0123456789\
0123456789\
0123456789\
0123456789\
\
0123456789\
0123456789\
0123456789\
0123456789\
0123456789\
\
0123456789\
0123456789\
0123456789\
0123456789\
0123456789\
\
0123456789\
0123456789\
0123456789\
0123456789\
0123456789\
\
0123456789\
0123456789\
0123456789\
0123456789\
0123456789\
\
01234

run_check $SUDO_HELPER "$TOP/btrfs" filesystem label "$TEST_MNT" "$longlabel"
run_check $SUDO_HELPER "$TOP/btrfs" filesystem label "$TEST_MNT"
# 256, must fail
run_mustfail "label 256 bytes long succeeded" \
	$SUDO_HELPER "$TOP/btrfs" filesystem label "$TEST_MNT" "$longlabel"5
run_check $SUDO_HELPER "$TOP/btrfs" filesystem label "$TEST_MNT"
run_mustfail "label 2 * 255 bytes long succeeded" \
	$SUDO_HELPER "$TOP/btrfs" filesystem label "$TEST_MNT" "$longlabel$longlabel"
run_check $SUDO_HELPER "$TOP/btrfs" filesystem label "$TEST_MNT"

cd ..

run_check_umount_test_dev
