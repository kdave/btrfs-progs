#!/bin/bash
# Make sure the migrated range doesn't cause csum errors

source "$TEST_TOP/common" || exit
source "$TEST_TOP/common.convert" || exit

setup_root_helper
prepare_test_dev 10G

check_global_prereq mkfs.ext4
check_prereq btrfs-convert
check_prereq btrfs

run_check mkfs.ext4 -b 64K -F "$TEST_DEV"
run_check $SUDO_HELPER "$TOP/btrfs-convert" --nodesize 64K "$TEST_DEV"
run_check $SUDO_HELPER "$TOP/btrfs" check --check-data-csum "$TEST_DEV"
