#!/bin/bash
# check that sanitized names with matching crc do not contain unprintable
# characters, namely 0x7f

source "$TEST_TOP/common"

check_prereq mkfs.btrfs
check_prereq btrfs

prepare_test_dev

run_check_mkfs_test_dev
run_check_mount_test_dev
run_check $SUDO_HELPER chmod a+rw "$TEST_MNT"

# known to produce char 0x7f == 127
touch "$TEST_MNT/|5gp!"

run_check_umount_test_dev

_mktemp_local img
_mktemp_local img.restored
_mktemp_local img.dump
run_check $SUDO_HELPER "$TOP/btrfs-image" -ss "$TEST_DEV" img
run_check $SUDO_HELPER "$TOP/btrfs-image" -r img img.restored
run_check_stdout $SUDO_HELPER "$TOP/btrfs" inspect-internal dump-tree img.restored > img.dump

ch7f=$(echo -en '\x7f')
if grep -q "$ch7f" img.dump; then
	_fail "found char 0x7f in the sanitized names"
fi

rm -f -- img img.restored img.dump
