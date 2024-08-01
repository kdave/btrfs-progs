#!/bin/bash
#
# Test if "mkfs.btrfs --rootdir" would handle a rootdir with subdirectories
# on another fs.

source "$TEST_TOP/common" || exit

setup_root_helper

# Here we need 3 devices, one for the rootdir, one for a subdirectory of
# rootdir. This is to ensure we have conflicting inode numbers among the
# two filesystems.
# The last device is the for the mkfs.
setup_loopdevs 3
prepare_loopdevs

dev1=${loopdevs[1]}
dev2=${loopdevs[2]}
dev3=${loopdevs[3]}
tmpdir=$(_mktemp_dir mkfs-rootdir-cross-mount)

run_check $SUDO_HELPER "$TOP/mkfs.btrfs" -f "$dev1"
run_check $SUDO_HELPER "$TOP/mkfs.btrfs" -f "$dev2"

# Populate both filesystems with the same contents. So that we're ensured
# to have conflicting inode numbers.
for i in "$dev1" "$dev2"; do
	run_check $SUDO_HELPER mount -t btrfs "$i" "$tmpdir"
	run_check mkdir "$tmpdir/dir1" "$tmpdir/dir2"
	run_check touch "$tmpdir/file1" "$tmpdir/file2"
	run_check $SUDO_HELPER umount "$tmpdir"
done

run_check $SUDO_HELPER mount -t btrfs "$dev1" "$tmpdir"
run_check $SUDO_HELPER mount -t btrfs "$dev2" "$tmpdir/dir1"

# The old rootdir implementation relies on the st_ino from the host fs, but
# doesn't do any cross-mount checks, it would result conflicting inode numbers
# and fail.
run_check "$TOP/mkfs.btrfs" --rootdir "$tmpdir" -f "$dev3"
run_check $SUDO_HELPER umount "$tmpdir/dir1"
run_check $SUDO_HELPER umount "$tmpdir"
run_check "$TOP/btrfs" check "$dev3"

rm -rf -- "$tmpdir"
cleanup_loopdevs
