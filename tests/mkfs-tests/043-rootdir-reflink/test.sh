#!/bin/bash
# Make sure the --reflink option works.

source "$TEST_TOP/common" || exit

check_prereq mkfs.btrfs
check_prereq btrfs

setup_root_helper
prepare_test_dev

verify_fs_image()
{
	# Verify the content of the new fs image
	run_check $SUDO_HELPER mount "$TEST_MNT/image" "$TEST_MNT/rootdir"
	run_check $SUDO_HELPER md5sum -c "$TEST_MNT/md5sum"
	run_check $SUDO_HELPER umount "$TEST_MNT/rootdir"
	run_check $SUDO_HELPER "$TOP/btrfs" check --check-data-csum "$TEST_MNT/image"
	# Wait for the loopdev of "$TEST_MNT/image" to be completely removed, so that we can
	# unmount "$TEST_MNT"
	run_mayfail $SUDO_HELPER udevadm settle
}

run_check_mkfs_test_dev
run_check_mount_test_dev -o max_inline=0

# Populate the rootdir
run_check $SUDO_HELPER mkdir -p "$TEST_MNT/rootdir"

# A regular 16M file, exercising the aligned block handling.
run_check $SUDO_HELPER dd if=/dev/urandom of="$TEST_MNT/rootdir/aligned" \
	bs=1M count=16 status=noxfer > /dev/null 2>&1

# A regular 65K file, mixing aligned block handling and the tailing partial block
# handling.
run_check $SUDO_HELPER dd if=/dev/urandom of="$TEST_MNT/rootdir/multi_block_unaligned" \
	bs=1K count=65 status=noxfer > /dev/null 2>&1

# A regular 2K file, should be inlined inside the fs image.
run_check $SUDO_HELPER dd if=/dev/urandom of="$TEST_MNT/rootdir/partial_block" \
	bs=1K count=2 status=noxfer > /dev/null 2>&1

# A hole
run_check $SUDO_HELPER truncate -s 37k "$TEST_MNT/rootdir/hole"

run_check sync

# Generate the md5sum for all above files.
run_check_stdout $SUDO_HELPER find "$TEST_MNT/rootdir" -type f -exec md5sum {} \+ > "$TEST_MNT/md5sum"

run_check $SUDO_HELPER "$TOP/mkfs.btrfs" -f "$TEST_MNT/image" --reflink --rootdir "$TEST_MNT/rootdir"
verify_fs_image

run_check $SUDO_HELPER "$TOP/mkfs.btrfs" -f "$TEST_MNT/image" --rootdir "$TEST_MNT/rootdir"
verify_fs_image

run_check $SUDO_HELPER umount "$TEST_MNT"
