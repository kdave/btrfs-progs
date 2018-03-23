#!/bin/bash
#
# verify that convert rollback finds the ext2_subvolume intact and fails if it
# was partially deleted

source "$TEST_TOP/common"

check_prereq btrfs-convert
check_prereq btrfs

setup_root_helper
prepare_test_dev

run_check truncate -s 2G "$TEST_DEV"
run_check mkfs.ext4 -F "$TEST_DEV"
run_check "$TOP/btrfs-convert" "$TEST_DEV"
run_check $SUDO_HELPER "$TOP/btrfs" inspect-internal dump-tree "$TEST_DEV"
run_check_mount_test_dev
run_check $SUDO_HELPER "$TOP/btrfs" subvolume delete -c "$TEST_MNT/ext2_saved"
run_check_umount_test_dev
run_check $SUDO_HELPER "$TOP/btrfs" inspect-internal dump-tree "$TEST_DEV"
run_check_stdout "$TOP/btrfs-convert" --rollback "$TEST_DEV" |
	grep -q 'is it deleted' || _fail "unexpected rollback"

exit 0
