#!/bin/bash
# make sure that 'missing' is accepted for device deletion

source $TOP/tests/common

check_prereq btrfs-show-super
check_prereq mkfs.btrfs
check_prereq btrfs

ndevs=4
declare -a devs
dev1=
devtodel=

setup_root_helper

prepare_devices()
{
	for i in `seq $ndevs`; do
		touch img$i
		chmod a+rw img$i
		truncate -s0 img$i
		truncate -s2g img$i
		devs[$i]=`run_check_stdout $SUDO_HELPER losetup --find --show img$i`
	done
}

cleanup_devices()
{
	for dev in ${devs[@]}; do
		run_mayfail $SUDO_HELPER losetup -d $dev
	done
	for i in `seq $ndevs`; do
		truncate -s0 img$i
	done
	run_check $SUDO_HELPER losetup --list
}

test_do_mkfs()
{
	run_check $SUDO_HELPER $TOP/mkfs.btrfs -f $@ ${devs[@]}
	run_check $TOP/btrfs-show-super $dev1
	run_check $SUDO_HELPER $TOP/btrfs check $dev1
	run_check $TOP/btrfs filesystem show
}

test_wipefs()
{
	run_check wipefs -a $devtodel
	run_check $SUDO_HELPER losetup -d $devtodel
	run_check losetup -a
	run_check $TOP/btrfs filesystem show
}
test_delete_missing()
{
	run_check_mount_test_dev -o degraded
	run_check $SUDO_HELPER $TOP/btrfs filesystem show $TEST_MNT
	run_check $SUDO_HELPER $TOP/btrfs device delete missing $TEST_MNT
	run_check $SUDO_HELPER $TOP/btrfs filesystem show $TEST_MNT
	run_check_umount_test_dev

	run_check_mount_test_dev
	local out
	out="$(run_check_stdout $SUDO_HELPER $TOP/btrfs filesystem show $TEST_MNT)"
	if echo "$out" | grep -q -- "$devtodel"; then
		_fail "device $devtodel not deleted"
	fi
	if echo "$out" | grep -q missing; then
		_fail "missing device still present"
	fi
	run_check_umount_test_dev
}

prepare_devices
dev1=${devs[1]}
devtodel=${devs[3]}
TEST_DEV=$dev1

test_do_mkfs
test_wipefs
test_delete_missing

cleanup_devices
