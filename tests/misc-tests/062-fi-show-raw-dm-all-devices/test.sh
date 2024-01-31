#!/bin/bash
# Verify that filesystem show -d on a raw device mapper path still recognizes
# the filesystem

source "$TEST_TOP/common" || exit

check_prereq mkfs.btrfs
check_global_prereq udevadm
check_dm_target_support linear

setup_root_helper
prepare_test_dev

# Randomize last 4 characters to prevent clashes of device name on the same system
chars=( {0..9} {a..z} {A..Z} )
rand=${chars[$RANDOM % 62]}${chars[$RANDOM % 62]}${chars[$RANDOM % 62]}${chars[$RANDOM % 62]}

# prep device
dmname=btrfs-test-device-"$rand"
dmdev="/dev/mapper/$dmname"

_mktemp_local img 2g

loopdev=`run_check_stdout $SUDO_HELPER losetup --find --show img`
run_check $SUDO_HELPER dmsetup create "$dmname" --table "0 1048576 linear $loopdev 0"

# Setting up the device may need some time to appear
run_check $SUDO_HELPER udevadm settle
if ! [ -b "$dmdev" ]; then
	_not_run "dm device created but not visible in /dev/mapper"
fi

dmraw=`readlink -f "$dmdev"`

# test
run_check $SUDO_HELPER "$TOP/mkfs.btrfs" -f "$@" "$dmdev"
run_check $SUDO_HELPER udevadm settle

run_check $SUDO_HELPER lsblk

run_check $SUDO_HELPER "$TOP/btrfs" filesystem show
run_check $SUDO_HELPER "$TOP/btrfs" filesystem show "$dmdev"
run_check $SUDO_HELPER "$TOP/btrfs" filesystem show --all-devices "$dmdev"
run_check $SUDO_HELPER "$TOP/btrfs" filesystem show "$dmraw"
run_check $SUDO_HELPER "$TOP/btrfs" filesystem show --all-devices "$dmraw"

# cleanup
run_check $SUDO_HELPER dmsetup remove "$dmname"
run_mayfail $SUDO_HELPER losetup -d "$loopdev"
run_check truncate -s0 img
rm img
