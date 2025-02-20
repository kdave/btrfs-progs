#!/bin/bash
# Print all nested subvolume paths as they're getting deleted

source "$TEST_TOP/common" || exit

setup_root_helper
prepare_test_dev

run_check_mkfs_test_dev
run_check_mount_test_dev
for path in						\
	"subvol1"					\
	"subvol1/subvol2"				\
	"subvol1/subvol2/subvol3"			\
	"subvol1/subvol2/subvol3/subvol4"		\
	"subvol1/subvol2/subvol3/subvol4/subvol5"; do

	run_check $SUDO_HELPER "$TOP/btrfs" subvolume create "$TEST_MNT/$path"
done

run_mustfail "deleted non-empty subvolume" \
	$SUDO_HELPER "$TOP/btrfs" subvolume delete "$TEST_MNT/subvol1"

if ! run_check_stdout $SUDO_HELPER "$TOP/btrfs" subvolume delete --recursive "$TEST_MNT/subvol1" |
	grep -q 'subvol[2345]'; then
	_fail "nested subvolumes not printed"
fi

run_check_umount_test_dev
