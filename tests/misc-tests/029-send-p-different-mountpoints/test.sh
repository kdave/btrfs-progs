#!/bin/bash
# test that send -p does not corrupt paths when send is using 2 different mount
# points

source "$TEST_TOP/common"

check_prereq btrfs
check_prereq mkfs.btrfs

setup_root_helper
prepare_test_dev

# we need two mount points, cannot nest the subvolume under TEST_MNT
SUBVOL_MNT="`pwd`/subvol_mnt"
# the 2nd mount directory is created here, add a fallback in case we're on NFS
run_mayfail $SUDO_HELPER mkdir -p "$SUBVOL_MNT" ||
	run_check mkdir -p "$SUBVOL_MNT"

run_check_mkfs_test_dev
run_check_mount_test_dev

run_check $SUDO_HELPER "$TOP/btrfs" subvolume create "$TEST_MNT/subv1"
run_check $SUDO_HELPER mount -t btrfs -o subvol=subv1 "$TEST_DEV" "$SUBVOL_MNT"

run_check $SUDO_HELPER "$TOP/btrfs" subvolume create "$TEST_MNT/test-subvol"
run_check $SUDO_HELPER "$TOP/btrfs" subvolume snapshot -r \
	"$TEST_MNT/test-subvol" "$SUBVOL_MNT/test-subvol-mnt-subvol"
run_check $SUDO_HELPER "$TOP/btrfs" subvolume snapshot -r \
	"$TEST_MNT/test-subvol" "$TEST_MNT/test-subvol-mnt-root"

run_mustfail_stdout "send -p on 2 mount points" \
	$SUDO_HELPER "$TOP/btrfs" send -f /dev/null -p \
	"$SUBVOL_MNT/test-subvol-mnt-subvol" "$TEST_MNT/test-subvol-mnt-root" \
	| _log_stdout \
	| grep -q "not on mount point: .*/mnt" \
	|| _fail "expected output not found, please check the logs"

# without a fix, this leads to a corrupted path, with something like:
#
# ERROR: open st-subvol-mnt-subvol failed. No such file or directory
#             ^^^^^^^^^^^^^^^^^^^^
# ERROR: could not resolve rootid for .../tests/mnt/subvol/test-subvol-mnt-subvol

# expected output:
# ERROR: not on mount point: .../tests/mnt/toplevel

run_check_umount_test_dev "$SUBVOL_MNT"
run_check_umount_test_dev "$TEST_MNT"

run_mayfail $SUDO_HELPER rmdir "$SUBVOL_MNT"
run_mayfail rmdir "$SUBVOL_MNT"

# don't propagate any potential error from run_mayfail()
exit 0
