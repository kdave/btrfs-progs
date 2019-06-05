#!/bin/bash
#
# test file name length limits for subvolumes

source "$TEST_TOP/common"

check_prereq mkfs.btrfs
check_prereq btrfs

setup_root_helper
prepare_test_dev

run_check_mkfs_test_dev
run_check_mount_test_dev
run_check $SUDO_HELPER chmod a+rw "$TEST_MNT"

cd "$TEST_MNT"

longname=\
0123456789\
0123456789\
0123456789\
0123456789\
0123456789\
\
0123456789\
0123456789\
0123456789\
0123456789\
0123456789\
\
0123456789\
0123456789\
0123456789\
0123456789\
0123456789\
\
0123456789\
0123456789\
0123456789\
0123456789\
0123456789\
\
0123456789\
0123456789\
0123456789\
0123456789\
0123456789\
\
01234

# subvolume name length limit test

# short name test
run_check $SUDO_HELPER "$TOP/btrfs" subvolume create subvol
# 255
run_check $SUDO_HELPER "$TOP/btrfs" subvolume create "$longname"
# 256, must fail
run_mustfail "subvolume with name 256 bytes long succeeded" \
	$SUDO_HELPER "$TOP/btrfs" subvolume create "$longname"5
# 255*2, must fail
run_mustfail "subvolume with name 2 * 255 bytes long succeeded" \
	$SUDO_HELPER "$TOP/btrfs" subvolume create "$longname$longname"

# snapshot name length limit test

run_check $SUDO_HELPER mkdir snaps

# short name test
run_check $SUDO_HELPER "$TOP/btrfs" subvolume snapshot subvol snaps/snap
# 255
run_check $SUDO_HELPER "$TOP/btrfs" subvolume snapshot subvol snaps/"$longname"
# 256, must fail
run_mustfail "snapshot with name 256 bytes long succeeded" \
	$SUDO_HELPER "$TOP/btrfs" subvolume snapshot subvol snaps/"$longname"5
# 255*2, must fail
run_mustfail "subvolume with name 2 * 255 bytes long succeeded" \
	$SUDO_HELPER "$TOP/btrfs" subvolume snapshot subvol snaps/"$longname$longname"

cd ..

run_check_umount_test_dev
