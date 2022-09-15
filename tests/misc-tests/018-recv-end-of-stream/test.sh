#!/bin/bash
#
# end of stream conditions: test that no instructions in a stream are still
# received, at least the header must be present

source "$TEST_TOP/common"

check_prereq mkfs.btrfs
check_prereq btrfs

setup_root_helper
prepare_test_dev

here=`pwd`

# All helpers can exercise various options passed to 'btrfs receive'

test_full_empty_stream() {
	local str

	str="$here/stream-full-empty.stream"
	run_check_mkfs_test_dev
	run_check_mount_test_dev

	cd "$TEST_MNT" || _fail "cannot chdir to TEST_MNT"

	run_check $SUDO_HELPER "$TOP/btrfs" subvolume create subv1
	run_check $SUDO_HELPER "$TOP/btrfs" subvolume snapshot -r subv1 subv1-snap

	_mktemp_local "$str"
	run_check $SUDO_HELPER "$TOP/btrfs" send -f "$str" subv1-snap

	cd "$here" || _fail "cannot chdir back to test directory"
	run_check_umount_test_dev

	run_check_mkfs_test_dev
	run_check_mount_test_dev
	run_check $SUDO_HELPER "$TOP/btrfs" receive "$@" -v -f "$str" "$TEST_MNT"
	run_check_umount_test_dev

	run_check rm -f -- "$str"
}

test_full_simple_stream() {
	local str

	str="$here/stream-full-simple.stream"
	run_check_mkfs_test_dev
	run_check_mount_test_dev

	cd "$TEST_MNT" || _fail "cannot chdir to TEST_MNT"

	run_check $SUDO_HELPER "$TOP/btrfs" subvolume create subv1
	for i in 1 2 3; do
		run_check $SUDO_HELPER dd if=/dev/zero of=subv1/file1_$i bs=1M count=1
	done

	run_check $SUDO_HELPER "$TOP/btrfs" subvolume snapshot -r subv1 subv1-snap

	_mktemp_local "$str"
	run_check $SUDO_HELPER "$TOP/btrfs" send -f "$str" subv1-snap

	cd "$here" || _fail "cannot chdir back to test directory"
	run_check_umount_test_dev

	run_check_mkfs_test_dev
	run_check_mount_test_dev
	run_check $SUDO_HELPER "$TOP/btrfs" receive "$@" -v -f "$str" "$TEST_MNT"
	run_check_umount_test_dev

	run_check rm -f -- "$str"
}

test_incr_empty_stream() {
	local fstr
	local istr

	fstr="$here/stream-full-empty.stream"
	istr="$here/stream-incr-empty.stream"
	run_check_mkfs_test_dev
	run_check_mount_test_dev

	cd "$TEST_MNT" || _fail "cannot chdir to TEST_MNT"

	run_check $SUDO_HELPER "$TOP/btrfs" subvolume create subv1
	run_check $SUDO_HELPER "$TOP/btrfs" subvolume snapshot -r subv1 subv1-snap
	run_check $SUDO_HELPER "$TOP/btrfs" subvolume snapshot -r subv1 subv2-snap

	_mktemp_local "$fstr"
	_mktemp_local "$istr"
	run_check $SUDO_HELPER "$TOP/btrfs" send -f "$fstr" subv1-snap
	run_check $SUDO_HELPER "$TOP/btrfs" send -p subv1-snap -f "$istr" subv2-snap

	cd "$here" || _fail "cannot chdir back to test directory"
	run_check_umount_test_dev

	run_check_mkfs_test_dev
	run_check_mount_test_dev
	run_check $SUDO_HELPER "$TOP/btrfs" receive "$@" -v -f "$fstr" "$TEST_MNT"
	run_check $SUDO_HELPER "$TOP/btrfs" receive "$@" -v -f "$istr" "$TEST_MNT"
	run_check_umount_test_dev

	run_check rm -f -- "$fstr" "$istr"
}

test_incr_simple_stream() {
	local str

	fstr="$here/stream-full-simple.stream"
	istr="$here/stream-incr-simple.stream"
	run_check_mkfs_test_dev
	run_check_mount_test_dev

	cd "$TEST_MNT" || _fail "cannot chdir to TEST_MNT"

	run_check $SUDO_HELPER "$TOP/btrfs" subvolume create subv1
	for i in 1 2 3; do
		run_check $SUDO_HELPER dd if=/dev/zero of=subv1/file1_$i bs=1M count=1
	done

	run_check $SUDO_HELPER "$TOP/btrfs" subvolume snapshot -r subv1 subv1-snap

	for i in 1 2 3; do
		run_check $SUDO_HELPER dd if=/dev/urandom of=subv1/file1_$i bs=1M count=1
	done

	run_check $SUDO_HELPER "$TOP/btrfs" subvolume snapshot -r subv1 subv2-snap

	_mktemp_local "$fstr"
	_mktemp_local "$istr"
	run_check $SUDO_HELPER "$TOP/btrfs" send -f "$fstr" subv1-snap
	run_check $SUDO_HELPER "$TOP/btrfs" send -p subv1-snap -f "$istr" subv2-snap

	cd "$here" || _fail "cannot chdir back to test directory"
	run_check_umount_test_dev

	run_check_mkfs_test_dev
	run_check_mount_test_dev
	run_check $SUDO_HELPER "$TOP/btrfs" receive "$@" -v -f "$fstr" "$TEST_MNT"
	run_check $SUDO_HELPER "$TOP/btrfs" receive "$@" -v -f "$istr" "$TEST_MNT"
	run_check_umount_test_dev

	run_check rm -f -- "$fstr" "$istr"
}

test_full_empty_stream
test_full_simple_stream
test_incr_empty_stream
test_incr_simple_stream

extra_opt=-e
test_full_empty_stream "$extra_opt"
test_full_simple_stream "$extra_opt"
test_incr_empty_stream "$extra_opt"
test_incr_simple_stream "$extra_opt"
