#!/bin/bash
#
# coverage of balance --full-balance

source "$TEST_TOP/common"

check_prereq mkfs.btrfs
check_prereq btrfs

setup_root_helper
prepare_test_dev

run_check_mkfs_test_dev
run_check_mount_test_dev

run_check $SUDO_HELPER "$TOP/btrfs" balance start --full-balance "$TEST_MNT"
run_check $SUDO_HELPER "$TOP/btrfs" balance start "$TEST_MNT"
run_check $SUDO_HELPER "$TOP/btrfs" balance --full-balance "$TEST_MNT"
run_check $SUDO_HELPER "$TOP/btrfs" balance "$TEST_MNT"

# grep below can't use -q else this could lead to SIGPIPE
run_check_stdout $SUDO_HELPER "$TOP/btrfs" balance start --background "$TEST_MNT" |
	grep -F "Full balance without filters requested." >/dev/null ||
	_fail "full balance warning not in the output"
run_mayfail $SUDO_HELPER "$TOP/btrfs" balance cancel "$TEST_MNT"

run_check_umount_test_dev
