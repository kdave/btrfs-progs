#!/bin/bash
#
# test label settings

source $TOP/tests/common

check_prereq mkfs.btrfs
check_prereq btrfs

setup_root_helper

run_check truncate -s 2G $IMAGE
run_check $TOP/mkfs.btrfs -L BTRFS-TEST-LABEL -f $IMAGE
run_check $SUDO_HELPER mount $IMAGE $TEST_MNT
run_check $SUDO_HELPER chmod a+rw $TEST_MNT

cd $TEST_MNT
run_check $SUDO_HELPER $TOP/btrfs filesystem label $TEST_MNT
# shortest label
run_check $SUDO_HELPER $TOP/btrfs filesystem label $TEST_MNT a
run_check $SUDO_HELPER $TOP/btrfs filesystem label $TEST_MNT
run_check $SUDO_HELPER $TOP/btrfs filesystem label $TEST_MNT ''

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

run_check $SUDO_HELPER $TOP/btrfs filesystem label $TEST_MNT "$longlabel"
run_check $SUDO_HELPER $TOP/btrfs filesystem label $TEST_MNT
# 256, must fail
run_mustfail "label 256 bytes long succeeded" \
	$SUDO_HELPER $TOP/btrfs filesystem label $TEST_MNT "$longlabel"5
run_check $SUDO_HELPER $TOP/btrfs filesystem label $TEST_MNT
run_mustfail "label 2 * 255 bytes long succeeded" \
	$SUDO_HELPER $TOP/btrfs filesystem label $TEST_MNT "$longlabel$longlabel"
run_check $SUDO_HELPER $TOP/btrfs filesystem label $TEST_MNT

cd ..

run_check $SUDO_HELPER umount $TEST_MNT
