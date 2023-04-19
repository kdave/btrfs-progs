#!/bin/bash
# test for 'filesystem show' on fresh local file

source "$TEST_TOP/common" || exit

check_prereq mkfs.btrfs
check_prereq btrfs

setup_root_helper

_mktemp_local img 3g
run_check $SUDO_HELPER "$TOP/mkfs.btrfs" -f img
run_check $SUDO_HELPER "$TOP/btrfs" filesystem show img

rm -f img
