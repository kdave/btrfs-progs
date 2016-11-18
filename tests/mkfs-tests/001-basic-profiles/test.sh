#!/bin/bash
# test various blockgroup profile combinations, use loop devices as block
# devices

source $TOP/tests/common

check_prereq mkfs.btrfs
check_prereq btrfs

ndevs=4
declare -a devs
dev1=

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
		run_check $SUDO_HELPER losetup -d $dev
	done
	for i in `seq $ndevs`; do
		truncate -s0 img$i
	done
	run_check $SUDO_HELPER losetup --all
}

test_get_info()
{
	run_check $SUDO_HELPER $TOP/btrfs inspect-internal dump-super $dev1
	run_check $SUDO_HELPER $TOP/btrfs check $dev1
	run_check $SUDO_HELPER mount $dev1 $TEST_MNT
	run_check $TOP/btrfs filesystem df $TEST_MNT
	run_check $SUDO_HELPER $TOP/btrfs filesystem usage $TEST_MNT
	run_check $SUDO_HELPER $TOP/btrfs device usage $TEST_MNT
	run_check $SUDO_HELPER umount "$TEST_MNT"
}
test_do_mkfs()
{
	run_check $SUDO_HELPER $TOP/mkfs.btrfs -f	\
		$@
}

test_mkfs_single()
{
	test_do_mkfs $@ $dev1
	test_get_info
}
test_mkfs_multi()
{
	test_do_mkfs $@ ${devs[@]}
	test_get_info
}

prepare_devices
dev1=${devs[1]}

test_mkfs_single
test_mkfs_single  -d  single  -m  single
test_mkfs_single  -d  single  -m  single  --mixed
test_mkfs_single  -d  single  -m  dup
test_mkfs_single  -d  dup     -m  single
test_mkfs_single  -d  dup     -m  dup
test_mkfs_single  -d  dup     -m  dup     --mixed

test_mkfs_multi
test_mkfs_multi   -d  single  -m  single
test_mkfs_multi   -d  single  -m  single  --mixed
test_mkfs_multi   -d  raid0   -m  raid0
test_mkfs_multi   -d  raid0   -m  raid0   --mixed
test_mkfs_multi   -d  raid1   -m  raid1
test_mkfs_multi   -d  raid1   -m  raid1   --mixed
test_mkfs_multi   -d  raid10  -m  raid10
test_mkfs_multi   -d  raid10  -m  raid10  --mixed
test_mkfs_multi   -d  raid5   -m  raid5
test_mkfs_multi   -d  raid5   -m  raid5   --mixed
test_mkfs_multi   -d  raid6   -m  raid6
test_mkfs_multi   -d  raid6   -m  raid6   --mixed
test_mkfs_multi   -d  dup     -m  dup
test_mkfs_multi   -d  dup     -m  dup     --mixed

cleanup_devices
