#!/bin/bash
# Verify --uuid option on ext2

source "$TEST_TOP/common"
source "$TEST_TOP/common.convert"

setup_root_helper
check_prereq btrfs-convert
check_global_prereq mke2fs
setup_loopdevs 1
prepare_loopdevs
# Convert helpers need the backing file, can't pass ${loopdevs[1]}
TEST_DEV=${loopdev_prefix}1

convert_test_prep_fs ext4 mke2fs -t ext4 -b 4096
run_check_umount_test_dev

read_btrfs_fsid() {
	run_check_stdout "$TOP/btrfs" inspect-internal dump-super "$TEST_DEV" |
		grep '^fsid' | awk '{print $2}'
}

read_ext2_fsid() {
	run_check_stdout dumpe2fs "$TEST_DEV" | grep "Filesystem UUID" | awk '{print $3}'
}

is_valid_uuid() {
	run_check uuidparse --noheadings "$1" | grep 'invalid' && false
}

uuid=$(read_ext2_fsid)
is_valid_uuid "$uuid"

run_check "$TOP/btrfs-convert" "$TEST_DEV"
is_valid_uuid $(read_btrfs_fsid)
run_check "$TOP/btrfs-convert" --rollback "$TEST_DEV"

run_check "$TOP/btrfs-convert" --uuid new "$TEST_DEV"
is_valid_uuid $(read_btrfs_fsid)
run_check "$TOP/btrfs-convert" --rollback "$TEST_DEV"

run_check "$TOP/btrfs-convert" --uuid copy "$TEST_DEV"
btrfs_fsid=$(read_btrfs_fsid)
is_valid_uuid "$btrfs_fsid"
if ! [ "$uuid" = "$btrfs_fsid" ]; then
	_fail "copied UUID mismatch"
fi
run_check "$TOP/btrfs-convert" --rollback "$TEST_DEV"

newuuid=$(run_check_stdout uuidgen)
run_check "$TOP/btrfs-convert" --uuid "$newuuid" "$TEST_DEV"
btrfs_fsid=$(read_btrfs_fsid)
is_valid_uuid "$btrfs_fsid"
if ! [ "$newuuid" = "$btrfs_fsid" ] ; then
	_fail "user-defined UUID mismatch"
fi
run_check "$TOP/btrfs-convert" --rollback "$TEST_DEV"

run_mustfail "invalid UUID passed" \
	"$TOP/btrfs-convert" --uuid invalid "$TEST_DEV"

cleanup_loopdevs
