#!/bin/bash
#
# Make sure "btrfs check" won't report false alerts on sprouted filesystems
#

source "$TEST_TOP/common"

check_prereq btrfs
check_prereq mkfs.btrfs
check_prereq btrfstune
check_global_prereq losetup

setup_loopdevs 2
prepare_loopdevs
dev1=${loopdevs[1]}
dev2=${loopdevs[2]}
TEST_DEV=$dev1

setup_root_helper

run_check $SUDO_HELPER "$TOP/mkfs.btrfs" -f "$dev1"
run_check $SUDO_HELPER "$TOP/btrfstune" -S 1 "$dev1"
run_check_mount_test_dev
run_check $SUDO_HELPER "$TOP/btrfs" device add -f "$dev2" "$TEST_MNT"

# Here we can not use umount helper, as it uses the seed device to do the
# umount. We need to manually unmout using the mount point
run_check $SUDO_HELPER umount "$TEST_MNT"

seed_output=$(_mktemp btrfs-progs-seed-check-stdout.XXXXXX)
sprouted_output=$(_mktemp btrfs-progs-sprouted-check-stdout.XXXXXX)

# The false alerts are just warnings, so we need to save and filter
# the output
run_check_stdout "$TOP/btrfs" check "$dev1" >> "$seed_output"
run_check_stdout "$TOP/btrfs" check "$dev2" >> "$sprouted_output"

# There should be no warning for both seed and sprouted fs
if grep -q "WARNING" "$seed_output"; then
	cleanup_loopdevs
	rm -f -- "$seed_output" "$sprouted_output"
	_fail "false alerts detected for seed fs"
fi
if grep -q "WARNING" "$sprouted_output"; then
	cleanup_loopdevs
	rm -f -- "$seed_output" "$sprouted_output"
	_fail "false alerts detected for sprouted fs"
fi

cleanup_loopdevs
rm -f -- "$seed_output" "$sprouted_output"
