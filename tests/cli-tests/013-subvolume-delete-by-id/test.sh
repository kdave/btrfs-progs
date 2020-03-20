#!/bin/bash
# Test deletion of subvolume specified by id

source "$TEST_TOP/common"

check_prereq mkfs.btrfs
check_prereq btrfs

setup_root_helper
prepare_test_dev

run_check_mkfs_test_dev
run_check_mount_test_dev

run_check $SUDO_HELPER "$TOP/btrfs" subvolume create "$TEST_MNT/subvol1"
run_check $SUDO_HELPER "$TOP/btrfs" subvolume create "$TEST_MNT/subvol2"
run_check $SUDO_HELPER "$TOP/btrfs" subvolume create "$TEST_MNT/subvol3"

# Expected failures
run_mustfail "subvolume delete --subvolid expects an integer" \
	$SUDO_HELPER "$TOP/btrfs" subvolume delete --subvolid aaa "$TEST_MNT"

run_mustfail "subvolume delete --subvolid with non-existent subvolume" \
	$SUDO_HELPER "$TOP/btrfs" subvolume delete --subvolid 999 "$TEST_MNT"

run_mustfail "subvolume delete --subvolid expects only one extra argument, the mountpoint" \
	$SUDO_HELPER "$TOP/btrfs" subvolume delete --subvolid 256 "$TEST_MNT" "$TEST_MNT"

# Delete the recently created subvol using the subvolid
# (First run is conditional to check for kernel support)
out=$(run_mayfail_stdout $SUDO_HELPER "$TOP/btrfs" subvolume delete --subvolid 256 "$TEST_MNT")
ret="$?"
run_check_umount_test_dev

if [ "$ret" != 0 ]; then
	if echo "$out" | grep -q 'Inappropriate ioctl for device'; then
		_not_run "subvolume delete --subvolid not supported"
	fi
	_fail "$out"
fi

run_check_mount_test_dev -o subvol=subvol2
# When the subvolume subvol3 is mounted, subvol2 is not reachable by the
# current mount point, but "subvolume delete --subvolid" should be able to
# delete it
run_check $SUDO_HELPER "$TOP/btrfs" subvolume delete --subvolid 258 "$TEST_MNT"

run_check_umount_test_dev
