#!/bin/bash
# Make sure the created btrfs with rootdir has its content matches its source

source "$TEST_TOP/common" || exit

check_prereq mkfs.btrfs
check_prereq btrfs
check_prereq fssum
check_prereq fsstress

if ! [ -f "/sys/fs/btrfs/features/supported_sectorsizes" ]; then
	_not_run "kernel support for different block sizes missing"
fi

setup_root_helper
prepare_test_dev

fssum_prog="$INTERNAL_BIN/fssum"
fsstress_prog="$INTERNAL_BIN/fsstress"
tmp=$(_mktemp_dir mkfs-rootdir)

mkdir "$tmp/rootdir"
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

run_check $SUDO_HELPER dd if=/dev/urandom of="$tmp/rootdir/large" bs=1M count=32
run_check $SUDO_HELPER truncate -s 512M "$tmp/rootdir/sparse"
run_check $SUDO_HELPER dd if=/dev/urandom of="$tmp/rootdir/sparse" bs=1M count=1 conv=notrunc seek=128
run_check $SUDO_HELPER $fsstress_prog -w -n 256 -d "$tmp/rootdir/"
run_check $SUDO_HELPER $fssum_prog -n -d -f -w "$tmp/fssum" "$tmp/rootdir"

workload()
{
	run_check_mkfs_test_dev -s $blocksize --rootdir "$tmp/rootdir" $@
	run_check $SUDO_HELPER "$TOP/btrfs" check "$TEST_DEV"
	run_check_mount_test_dev

	run_check $SUDO_HELPER $fssum_prog -r "$tmp/fssum" "$TEST_MNT/"
	run_check_umount_test_dev
}

workload -O no-holes
workload -O ^no-holes

run_check rm -rf -- "$tmp"
