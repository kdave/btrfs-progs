#!/bin/bash
# Verify btrfstune for enabling and removing simple quotas

source "$TEST_TOP/common" || exit
source "$TEST_TOP/common.convert" || exit

check_experimental_build
setup_root_helper
prepare_test_dev

# Create the fs without simple quota
run_check_mkfs_test_dev
run_check_mount_test_dev
populate_fs
run_check_umount_test_dev
# Enable simple quotas
run_check $SUDO_HELPER "$TOP/btrfstune" --enable-simple-quota "$TEST_DEV"
run_check_mount_test_dev
run_check $SUDO_HELPER dd if=/dev/zero of="$TEST_MNT"/file2 bs=1M count=1
run_check_umount_test_dev
run_check $SUDO_HELPER "$TOP/btrfs" check "$TEST_DEV"

# Populate new fs with simple quotas enabled
run_check_mkfs_test_dev -O squota
run_check_mount_test_dev
populate_fs
run_check_umount_test_dev
# Remove simple quotas
run_check $SUDO_HELPER "$TOP/btrfstune" --remove-simple-quota "$TEST_DEV"
run_check_mount_test_dev
run_check $SUDO_HELPER dd if=/dev/zero of="$TEST_MNT"/file3 bs=1M count=1
run_check_umount_test_dev
run_check $SUDO_HELPER "$TOP/btrfs" check "$TEST_DEV"
