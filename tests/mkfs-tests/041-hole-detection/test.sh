#!/bin/bash
# Test basic hole detection features

source "$TEST_TOP/common" || exit

check_prereq mkfs.btrfs
check_prereq btrfs
check_global_prereq du

if ! [ -f "/sys/fs/btrfs/features/supported_sectorsizes" ]; then
	_not_run "kernel support for different block sizes missing"
fi

setup_root_helper
prepare_test_dev

tmp=$(_mktemp_dir mkfs-rootdir)

# Get the fs block size, normally it's page size (using tmpfs for /tmp),
# but it can still be other values if /tmp is on a regular fs.
blocksize=$(stat -f -c %S "$tmp")

blocksize_supported=false
for bs in $(cat /sys/fs/btrfs/features/supported_sectorsizes); do
	if [ "$blocksize" == "$bs" ]; then
		blocksize_supported=true
	fi
done

if [ "$blocksize_supported" != "true" ]; then
	_not_run "kernel support for mounting blocksize $blocksize is missing"
fi

run_check truncate -s 1M "$tmp/full_hole"
full_hole_before=$(run_check_stdout md5sum "$tmp/full_hole" | awk '{print $1}')
run_check dd if=/dev/urandom of="$tmp/middle_data" bs=$blocksize seek=16 count=1
run_check truncate -s $(($blocksize * 32)) "$tmp/middle_data"
middle_data_before=$(run_check_stdout md5sum "$tmp/middle_data" | awk '{print $1}')
run_check dd if=/dev/urandom of="$tmp/middle_hole" bs=$blocksize count=1
run_check dd if=/dev/urandom of="$tmp/middle_hole" bs=$blocksize count=1 seek=16
middle_hole_before=$(run_check_stdout md5sum "$tmp/middle_hole" | awk '{print $1}')

run_check_mkfs_test_dev -s $blocksize --rootdir "$tmp"
run_check $SUDO_HELPER "$TOP/btrfs" check "$TEST_DEV"
run_check_mount_test_dev

# There are only 3 blocks written, thus 'du' should only report such 3 blocks used.
blocks=$(run_check_stdout du -B $blocksize "$TEST_MNT" | awk '{print $1}')

if [ "$blocks" != "3" ]; then
	_fail "Unexpected number of blocks written, has $blocks expect 3"
fi

full_hole_after=$(run_check_stdout md5sum "$TEST_MNT/full_hole" | awk '{print $1}')
middle_data_after=$(run_check_stdout md5sum "$TEST_MNT/middle_data" | awk '{print $1}')
middle_hole_after=$(run_check_stdout md5sum "$TEST_MNT/middle_hole" | awk '{print $1}')

if [ "$full_hole_before" != "$full_hole_after" ]; then
	_fail "full_hole content changed"
fi
if [ "$middle_data_before" != "$middle_data_after" ]; then
	_fail "middle_data content changed"
fi
if [ "$middle_hole_before" != "$middle_hole_after" ]; then
	_fail "middle_hole content changed"
fi

run_check_umount_test_dev
run_check rm -rf -- "$tmp"
