#!/bin/bash
# Print sizes with all supported suffixes

source "$TEST_TOP/common" || exit

check_prereq mkfs.btrfs
check_prereq btrfs

setup_root_helper
prepare_test_dev

run_check_mkfs_test_dev
run_check_mount_test_dev

# Generate some data
run_check $SUDO_HELPER dd if=/dev/zero bs=1M count=10 of="$TEST_MNT"/file21
run_check $SUDO_HELPER dd if=/dev/zero bs=1M count=10 of="$TEST_MNT"/file22
run_check $SUDO_HELPER dd if=/dev/zero bs=1M count=10 of="$TEST_MNT"/file1
run_check $SUDO_HELPER "$TOP/btrfs" subvolume create "$TEST_MNT"/subv1
run_check $SUDO_HELPER dd if=/dev/zero bs=1M count=10 of="$TEST_MNT"/subv1/file2
run_check $SUDO_HELPER "$TOP/btrfs" subvolume snapshot "$TEST_MNT"/subv1 "$TEST_MNT"/snap1
run_check $SUDO_HELPER dd if=/dev/zero bs=1M count=10 of="$TEST_MNT"/snap1/file3

run_check "$TOP/btrfs" filesystem df "$TEST_MNT"
run_check "$TOP/btrfs" filesystem df -b "$TEST_MNT"
run_check "$TOP/btrfs" filesystem df --raw "$TEST_MNT"
run_check "$TOP/btrfs" filesystem df -h "$TEST_MNT"
run_check "$TOP/btrfs" filesystem df --human-readable "$TEST_MNT"
run_check "$TOP/btrfs" filesystem df -H "$TEST_MNT"
run_check "$TOP/btrfs" filesystem df --iec "$TEST_MNT"
run_check "$TOP/btrfs" filesystem df --si "$TEST_MNT"

run_check "$TOP/btrfs" filesystem df -k "$TEST_MNT"
run_check "$TOP/btrfs" filesystem df --kbytes "$TEST_MNT"
run_check "$TOP/btrfs" filesystem df -m "$TEST_MNT"
run_check "$TOP/btrfs" filesystem df --mbytes "$TEST_MNT"
run_check "$TOP/btrfs" filesystem df -g "$TEST_MNT"
run_check "$TOP/btrfs" filesystem df --gbytes "$TEST_MNT"
run_check "$TOP/btrfs" filesystem df -t "$TEST_MNT"
run_check "$TOP/btrfs" filesystem df --tbytes "$TEST_MNT"

run_check "$TOP/btrfs" filesystem df --iec -k "$TEST_MNT"
run_check "$TOP/btrfs" filesystem df --iec --kbytes "$TEST_MNT"
run_check "$TOP/btrfs" filesystem df --iec -m "$TEST_MNT"
run_check "$TOP/btrfs" filesystem df --iec --mbytes "$TEST_MNT"
run_check "$TOP/btrfs" filesystem df --iec -g "$TEST_MNT"
run_check "$TOP/btrfs" filesystem df --iec --gbytes "$TEST_MNT"
run_check "$TOP/btrfs" filesystem df --iec -t "$TEST_MNT"
run_check "$TOP/btrfs" filesystem df --iec --tbytes "$TEST_MNT"

run_check "$TOP/btrfs" filesystem df --si -k "$TEST_MNT"
run_check "$TOP/btrfs" filesystem df --si --kbytes "$TEST_MNT"
run_check "$TOP/btrfs" filesystem df --si -m "$TEST_MNT"
run_check "$TOP/btrfs" filesystem df --si --mbytes "$TEST_MNT"
run_check "$TOP/btrfs" filesystem df --si -g "$TEST_MNT"
run_check "$TOP/btrfs" filesystem df --si --gbytes "$TEST_MNT"
run_check "$TOP/btrfs" filesystem df --si -t "$TEST_MNT"
run_check "$TOP/btrfs" filesystem df --si --tbytes "$TEST_MNT"

run_check_umount_test_dev
