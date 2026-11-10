#!/bin/bash
# Receive must fail when the clone source subvolume is outside the subvolume
# that is mounted as the receive target

source "$TEST_TOP/common" || exit

check_prereq mkfs.btrfs
check_prereq btrfs

setup_root_helper
prepare_test_dev

tmp=$(_mktemp_dir send-stream)

run_check_mkfs_test_dev
run_check_mount_test_dev

BLOCK_SIZE=$(stat -f -c %S "$TEST_MNT")

run_check $SUDO_HELPER "$TOP/btrfs" subvolume create "$TEST_MNT/foo"
run_check $SUDO_HELPER "$TOP/btrfs" subvolume create "$TEST_MNT/bar"
run_check $SUDO_HELPER dd if=/dev/urandom of="$TEST_MNT/foo/file" \
	bs="$BLOCK_SIZE" count=32
run_check $SUDO_HELPER "$TOP/btrfs" subvolume snapshot -r "$TEST_MNT/foo" \
	"$TEST_MNT/foo.0"
run_check $SUDO_HELPER "$TOP/btrfs" subvolume snapshot -r "$TEST_MNT/bar" \
	"$TEST_MNT/bar.0"
run_check $SUDO_HELPER cp --reflink=always "$TEST_MNT/foo.0/file" \
	"$TEST_MNT/bar/file"
run_check $SUDO_HELPER "$TOP/btrfs" subvolume snapshot -r "$TEST_MNT/bar" \
	"$TEST_MNT/bar.1"

# bar.1/file is sent as a clone of foo.0/file
run_check $SUDO_HELPER "$TOP/btrfs" send -f "$tmp/foo.0.stream" "$TEST_MNT/foo.0"
run_check $SUDO_HELPER "$TOP/btrfs" send -f "$tmp/bar.0.stream" "$TEST_MNT/bar.0"
run_check $SUDO_HELPER "$TOP/btrfs" send -p "$TEST_MNT/bar.0" -c "$TEST_MNT/foo.0" \
	-f "$tmp/bar.1.stream" "$TEST_MNT/bar.1"
run_check_umount_test_dev

# Receive foo.0 into the top level, then bar.0 and bar.1 with only the
# subvolume dest mounted, from where foo.0 cannot be reached
run_check_mkfs_test_dev
run_check_mount_test_dev
run_check $SUDO_HELPER "$TOP/btrfs" subvolume create "$TEST_MNT/dest"
run_check $SUDO_HELPER "$TOP/btrfs" receive -f "$tmp/foo.0.stream" "$TEST_MNT"
run_check_umount_test_dev
run_check_mount_test_dev -o subvol=/dest
run_check $SUDO_HELPER "$TOP/btrfs" receive -f "$tmp/bar.0.stream" "$TEST_MNT"

run_mustfail "receive succeeded with an unreachable clone source" \
	$SUDO_HELPER "$TOP/btrfs" receive -f "$tmp/bar.1.stream" "$TEST_MNT"

run_check_umount_test_dev
run_check $SUDO_HELPER rm -rf -- "$tmp"
