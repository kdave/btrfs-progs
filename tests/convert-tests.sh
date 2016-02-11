#!/bin/bash
#
# convert ext2/3/4 images to btrfs images, and make sure the results are
# clean.
#

unset TOP
unset LANG
LANG=C
SCRIPT_DIR=$(dirname $(readlink -f $0))
TOP=$(readlink -f $SCRIPT_DIR/../)
RESULTS="$TOP/tests/convert-tests-results.txt"

source $TOP/tests/common

rm -f $RESULTS

setup_root_helper
prepare_test_dev 256M

CHECKSUMTMP=$(mktemp --tmpdir btrfs-progs-convert.XXXXXXXXXX)

convert_test() {
	local features
	local nodesize

	features="$1"
	shift

	if [ -z "$features" ]; then
		echo "    [TEST/conv]   $1, btrfs defaults"
	else
		echo "    [TEST/conv]   $1, btrfs $features"
	fi
	nodesize=$2
	shift 2
	echo "creating ext image with: $*" >> $RESULTS
	# TEST_DEV not removed as the file might have special permissions, eg.
	# when test image is on NFS and would not be writable for root
	run_check truncate -s 0 $TEST_DEV
	# 256MB is the smallest acceptable btrfs image.
	run_check truncate -s 256M $TEST_DEV
	run_check $* -F $TEST_DEV

	# create a file to check btrfs-convert can convert regular file
	# correct
	run_check_mount_test_dev
	run_check $SUDO_HELPER dd if=/dev/zero of=$TEST_MNT/test bs=$nodesize \
		count=1 1>/dev/null 2>&1
	run_check_stdout md5sum $TEST_MNT/test > $CHECKSUMTMP
	run_check_umount_test_dev

	run_check $TOP/btrfs-convert ${features:+-O "$features"} -N "$nodesize" $TEST_DEV
	run_check $TOP/btrfs check $TEST_DEV
	run_check $TOP/btrfs-show-super $TEST_DEV

	run_check_mount_test_dev
	run_check_stdout md5sum -c $CHECKSUMTMP |
		grep -q 'OK' || _fail "file validation failed."
	run_check_umount_test_dev
}

if ! [ -z "$TEST" ]; then
	echo "    [TEST/conv]   skipped all convert tests, TEST=$TEST"
	exit 0
fi

for feature in '' 'extref' 'skinny-metadata' 'no-holes'; do
	convert_test "$feature" "ext2 4k nodesize" 4096 mke2fs -b 4096
	convert_test "$feature" "ext3 4k nodesize" 4096 mke2fs -j -b 4096
	convert_test "$feature" "ext4 4k nodesize" 4096 mke2fs -t ext4 -b 4096
	convert_test "$feature" "ext2 8k nodesize" 8192 mke2fs -b 4096
	convert_test "$feature" "ext3 8k nodesize" 8192 mke2fs -j -b 4096
	convert_test "$feature" "ext4 8k nodesize" 8192 mke2fs -t ext4 -b 4096
	convert_test "$feature" "ext2 16k nodesize" 16384 mke2fs -b 4096
	convert_test "$feature" "ext3 16k nodesize" 16384 mke2fs -j -b 4096
	convert_test "$feature" "ext4 16k nodesize" 16384 mke2fs -t ext4 -b 4096
	convert_test "$feature" "ext2 32k nodesize" 32768 mke2fs -b 4096
	convert_test "$feature" "ext3 32k nodesize" 32768 mke2fs -j -b 4096
	convert_test "$feature" "ext4 32k nodesize" 32768 mke2fs -t ext4 -b 4096
	convert_test "$feature" "ext2 64k nodesize" 65536 mke2fs -b 4096
	convert_test "$feature" "ext3 64k nodesize" 65536 mke2fs -j -b 4096
	convert_test "$feature" "ext4 64k nodesize" 65536 mke2fs -t ext4 -b 4096
done

rm $CHECKSUMTMP
