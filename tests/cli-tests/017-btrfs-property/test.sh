#!/bin/bash
# test btrfs property commands

source "$TEST_TOP/common"

# compare with lsattr to make sure
check_global_prereq lsattr

setup_root_helper
prepare_test_dev

run_check_mkfs_test_dev
run_check_mount_test_dev

run_check $SUDO_HELPER touch "$TEST_MNT/file"
run_check $SUDO_HELPER "$TOP/btrfs" property set "$TEST_MNT/file" datacow no
run_check_stdout $SUDO_HELPER "$TOP/btrfs" property get "$TEST_MNT/file" datacow |
	grep -q "datacow=no" || _fail "datacow wasn't no"
run_check_stdout $SUDO_HELPER lsattr "$TEST_MNT/file" |
	grep -q -- "C.* " || _fail "lsattr didn't agree NOCOW flag is set"
run_check $SUDO_HELPER "$TOP/btrfs" property set "$TEST_MNT/file" datacow yes
run_check_stdout $SUDO_HELPER "$TOP/btrfs" property get "$TEST_MNT/file" datacow |
	grep -q "datacow=yes" || _fail "datacow wasn't yes"

run_check_umount_test_dev
