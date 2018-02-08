#!/bin/bash
# Check if btrfs-convert refuses to rollback the filesystem, and leave the fs
# and the convert image untouched

source "$TEST_TOP/common"
source "$TEST_TOP/common.convert"

setup_root_helper
prepare_test_dev
check_prereq btrfs-convert
check_global_prereq mke2fs

# convert_test_prep_fs() will create large enough file inside the test device,
# that's good enough for us to test rollback failure.
convert_test_prep_fs ext4 mke2fs -t ext4 -b 4096
run_check_umount_test_dev
convert_test_do_convert "" 4096

run_check_mount_test_dev

# Now the fs is converted, balance it so later rollback should fail
run_check $SUDO_HELPER "$TOP/btrfs" balance start --full-balance "$TEST_MNT"
run_check_umount_test_dev

# rollback should fail
run_mustfail "rollback fs after balance" "$TOP/btrfs-convert" -r "$TEST_DEV"

# Ensure the fs and convert image can pass the check
run_check "$TOP/btrfs" check "$TEST_DEV"

run_check_mount_test_dev
run_check $SUDO_HELPER e2fsck -fn "$TEST_MNT/ext2_saved/image"
run_check_umount_test_dev
