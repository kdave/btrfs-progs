#!/bin/bash
# test btrfs-image with a missing device (uses loop devices)
#
# - btrfs-image must not loop indefinetelly
# - btrfs-image will expectedly fail to produce the dump

source $TOP/tests/common

check_prereq btrfs-show-super
check_prereq btrfs-image
check_prereq mkfs.btrfs
check_prereq btrfs

ndevs=2
declare -a devs
dev1=
dev2=

setup_root_helper


# TODO: move the helpers to common

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

test_image_dump()
{
	run_check $SUDO_HELPER $TOP/btrfs check $dev1
	# the output file will be deleted
	run_mayfail $SUDO_HELPER $TOP/btrfs-image $dev1 /tmp/test-img.dump
}

test_run()
{
	run_check $SUDO_HELPER $TOP/mkfs.btrfs -f -d raid1 -m raid1 $dev1 $dev2

	# we need extents to trigger reading from all devices
	run_check $SUDO_HELPER mount $dev1 $TEST_MNT
	run_check $SUDO_HELPER dd if=/dev/zero of=$TEST_MNT/a bs=1M count=10
	run_check $SUDO_HELPER dd if=/dev/zero of=$TEST_MNT/b bs=4k count=1000 conv=sync
	run_check $SUDO_HELPER umount $TEST_MNT

	test_image_dump
	run_check btrfs fi show $dev1
	# create a degraded raid1 filesystem, check must succeed
	# btrfs-image must not loop
	run_mayfail wipefs -a $dev2
	run_check $SUDO_HELPER losetup -d $dev2
	run_check btrfs fi show $dev1

	test_image_dump
}

prepare_devices
dev1=${devs[1]}
dev2=${devs[2]}
test_run
cleanup_devices
