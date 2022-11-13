#!/bin/bash
#
# Make sure "btrfs check" can handle degraded raid5.
#

source "$TEST_TOP/common"

check_prereq btrfs
check_prereq mkfs.btrfs
check_global_prereq losetup
check_global_prereq wipefs

setup_loopdevs 3
prepare_loopdevs
dev1=${loopdevs[1]}
dev2=${loopdevs[2]}
dev3=${loopdevs[3]}

setup_root_helper

# Run 1: victim is dev1
run_check $SUDO_HELPER "$TOP/mkfs.btrfs" -f -m raid5 -d raid5 "${loopdevs[@]}"
run_check $SUDO_HELPER wipefs -fa $dev1
run_check $SUDO_HELPER "$TOP/btrfs" check $dev2

# Run 2: victim is dev2
run_check $SUDO_HELPER "$TOP/mkfs.btrfs" -f -m raid5 -d raid5 "${loopdevs[@]}"
run_check $SUDO_HELPER wipefs -fa $dev2
run_check $SUDO_HELPER "$TOP/btrfs" check $dev3

# Run 3: victim is dev3
run_check $SUDO_HELPER "$TOP/mkfs.btrfs" -f -m raid5 -d raid5 "${loopdevs[@]}"
run_check $SUDO_HELPER wipefs -fa $dev3
run_check $SUDO_HELPER "$TOP/btrfs" check $dev1

cleanup_loopdevs
