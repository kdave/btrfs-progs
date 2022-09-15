#!/bin/bash
# To check if "btrfs check" can detect metadata dump (restored by btrfs-image)
# and ignore --check-data-csum option

source "$TEST_TOP/common"

check_prereq btrfs
check_prereq mkfs.btrfs
check_prereq btrfs-image
setup_root_helper
prepare_test_dev

run_check_mkfs_test_dev
run_check_mount_test_dev

run_check $SUDO_HELPER dd if=/dev/urandom of="$TEST_MNT/file" bs=4k count=16
run_check_umount_test_dev

_mktemp_local restored_image
run_check $SUDO_HELPER "$TOP/btrfs-image" "$TEST_DEV" "restored_image"

# use prepare_test_dev() to wipe all existing data on $TEST_DEV
# so there is no way that restored image could have matching data csum
prepare_test_dev

run_check $SUDO_HELPER "$TOP/btrfs-image" -r "restored_image" "$TEST_DEV"

# Should not report any error
run_check "$TOP/btrfs" check --check-data-csum "$TEST_DEV"

rm -rf -- "restored_image"
