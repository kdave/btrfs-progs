#!/bin/bash
# Test "btrfs restore" on compressed inlined extents.

source "$TEST_TOP/common" || exit
source "$TEST_TOP/common.convert" || exit

check_prereq mkfs.btrfs
check_prereq btrfs
check_global_prereq md5sum

setup_root_helper
prepare_test_dev

tmp=$(_mktemp_dir "restore-destination")
run_check_mkfs_test_dev
run_check_mount_test_dev -o max_inline=4095
run_check $SUDO_HELPER dd if=/dev/zero of="$TEST_MNT/foobar" bs=2k count=1 status=noxfer
md5_before=$(run_check_stdout md5sum "$TEST_MNT/foobar" | cut -d ' ' -f 1)
run_check_umount_test_dev

run_check $SUDO_HELPER "$TOP/btrfs" restore "$TEST_DEV" "$tmp"
md5_after=$(run_check_stdout md5sum "$tmp/foobar" | cut -d ' ' -f 1)

rm -rf "$tmp"

if [ "$md5_before" != "$md5_after" ]; then
	_fail "md5 checksum mismatch"
fi
