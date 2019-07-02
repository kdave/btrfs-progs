#!/bin/bash
# check how deep does recursive 'fi du' go, currently it has to stop at
# mountpoint and can continue to subvolumes

source "$TEST_TOP/common"

check_prereq mkfs.btrfs
check_prereq btrfs

setup_root_helper
prepare_test_dev

run_check_mkfs_test_dev
run_check_mount_test_dev

run_check truncate -s 0 img2
run_check truncate -s 1G img2
chmod a+w img2

# create second mount with btrfs, create a file in the target mount path, the
# mount must hide that
run_check $SUDO_HELPER mkdir -p "$TEST_MNT"/mnt2
run_check $SUDO_HELPER dd if=/dev/zero bs=1M count=10 of="$TEST_MNT"/mnt2/hiddenfile

run_check $SUDO_HELPER "$TOP"/mkfs.btrfs -f img2
run_check $SUDO_HELPER mount -o loop img2 "$TEST_MNT"/mnt2
run_check $SUDO_HELPER dd if=/dev/zero bs=1M count=10 of="$TEST_MNT"/mnt2/file21
run_check $SUDO_HELPER dd if=/dev/zero bs=1M count=10 of="$TEST_MNT"/mnt2/file22

run_check $SUDO_HELPER dd if=/dev/zero bs=1M count=10 of="$TEST_MNT"/file1
run_check $SUDO_HELPER "$TOP/btrfs" subvolume create "$TEST_MNT"/subv1
run_check $SUDO_HELPER dd if=/dev/zero bs=1M count=10 of="$TEST_MNT"/subv1/file2
run_check $SUDO_HELPER "$TOP/btrfs" subvolume snapshot "$TEST_MNT"/subv1 "$TEST_MNT"/snap1
run_check $SUDO_HELPER dd if=/dev/zero bs=1M count=10 of="$TEST_MNT"/snap1/file3

run_check $SUDO_HELPER find "$TEST_MNT"
run_check_stdout $SUDO_HELPER "$TOP/btrfs" filesystem du "$TEST_MNT" |
	grep -q 'mnt2' && _fail "recursive du went to another filesystem"

run_check_stdout $SUDO_HELPER "$TOP/btrfs" filesystem du "$TEST_MNT"/mnt2 |
	grep -q 'hiddenfile' && _fail "du sees beneath mount point"

run_check $SUDO_HELPER umount "$TEST_MNT"/mnt2
run_check_umount_test_dev

rm -- img2
