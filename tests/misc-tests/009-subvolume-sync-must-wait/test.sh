#!/bin/bash
#
# Verify that subvolume sync waits until the subvolume is cleaned

source "$TEST_TOP/common"

check_prereq mkfs.btrfs
check_prereq btrfs

setup_root_helper

prepare_test_dev

run_check_mkfs_test_dev
run_check_mount_test_dev
run_check $SUDO_HELPER chmod a+rw "$TEST_MNT"

cd "$TEST_MNT"

for i in `seq 5`; do
	run_check dd if=/dev/zero of=file$i bs=1M count=10
done

for sn in `seq 4`;do
	run_check $SUDO_HELPER "$TOP/btrfs" subvolume snapshot . snap$sn
	for i in `seq 10`; do
		run_check dd if=/dev/zero of="snap$sn/file$i" bs=1M count=10
	done
done

run_check $SUDO_HELPER "$TOP/btrfs" subvolume list .
run_check $SUDO_HELPER "$TOP/btrfs" subvolume list -d .

idtodel=$(_get_subvolid snap3)

# delete, sync after some time
run_check $SUDO_HELPER "$TOP/btrfs" subvolume delete -c snap3
{ sleep 5; run_check "$TOP/btrfs" filesystem sync "$TEST_MNT"; } &

run_check $SUDO_HELPER "$TOP/btrfs" subvolume sync . "$idtodel"

if run_check_stdout $SUDO_HELPER "$TOP/btrfs" subvolume list -d . |
		grep -q "ID $idtodel.*DELETED"; then
	_fail "sync did not wait for the subvolume cleanup"
fi

run_check "$TOP/btrfs" filesystem sync "$TEST_MNT"
run_check $SUDO_HELPER "$TOP/btrfs" subvolume list -d .

wait
cd ..

run_check_umount_test_dev
