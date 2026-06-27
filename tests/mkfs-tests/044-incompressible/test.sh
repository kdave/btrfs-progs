#!/bin/bash
# Make sure the --compress option handles incompressible data correctly.

source "$TEST_TOP/common" || exit

check_prereq mkfs.btrfs
check_prereq btrfs

setup_root_helper
prepare_test_dev

workload()
{
	local comp=$1
	# Create the fs using the specified compression algo
	run_check $SUDO_HELPER "$TOP/mkfs.btrfs" -f "$TEST_MNT/image" --rootdir "$TEST_MNT/rootdir" --compress "$comp"

	# Verify the content of the new fs image
	run_check $SUDO_HELPER mount "$TEST_MNT/image" "$TEST_MNT/rootdir"
	run_check $SUDO_HELPER md5sum -c "$TEST_MNT/md5sum"
	run_check $SUDO_HELPER umount "$TEST_MNT/rootdir"
	run_check $SUDO_HELPER "$TOP/btrfs" check --check-data-csum "$TEST_MNT/image"
}

run_check_mkfs_test_dev
run_check_mount_test_dev -o max_inline=0

# Populate the rootdir
run_check $SUDO_HELPER mkdir -p "$TEST_MNT/rootdir"

# Create an incompressible file.
run_check $SUDO_HELPER dd if=/dev/urandom of="$TEST_MNT/rootdir/incompressible" \
	bs=1M count=16 status=noxfer > /dev/null 2>&1

run_check sync

# Generate the md5sum for all above files.
run_check_stdout $SUDO_HELPER find "$TEST_MNT/rootdir" -type f -exec md5sum {} \+ > "$TEST_MNT/md5sum"

workload lzo
workload zlib
workload zstd

# Wait for the loopdev of "$TEST_MNT/image" to be completely removed, so that we can
# unmount "$TEST_MNT"
run_mayfail $SUDO_HELPER udevadm settle
run_check $SUDO_HELPER umount "$TEST_MNT"
