#!/bin/bash
#
# convert ext2/3/4 images to btrfs images, and make sure the results are
# clean.
#

unset TOP
unset LANG
LANG=C
SCRIPT_DIR=$(dirname $(realpath $0))
TOP=$(realpath $SCRIPT_DIR/../)
TEST_MNT=${TEST_MNT:-$TOP/tests/mnt}
RESULTS="$TOP/tests/convert-tests-results.txt"
IMAGE="$TOP/tests/test.img"

source $TOP/tests/common

rm -f $RESULTS

setup_root_helper

convert_test() {
	echo "    [TEST]   $1"
	nodesize=$2
	shift 2
	echo "creating ext image with: $*" >> $RESULTS
	# IMAGE not removed as the file might have special permissions, eg.
	# when test image is on NFS and would not be writable for root
	run_check truncate -s 0 $IMAGE
	# 256MB is the smallest acceptable btrfs image.
	run_check truncate -s 256M $IMAGE
	run_check $* -F $IMAGE

	# create a file to check btrfs-convert can convert regular file
	# correct
	run_check $SUDO_HELPER mount $IMAGE $TEST_MNT
	run_check $SUDO_HELPER dd if=/dev/zero of=$TEST_MNT/test bs=$nodesize \
		count=1 1>/dev/null 2>&1
	run_check $SUDO_HELPER umount $TEST_MNT
	run_check $TOP/btrfs-convert -N "$nodesize" $IMAGE
	run_check $TOP/btrfs check $IMAGE
}

# btrfs-convert requires 4k blocksize.
convert_test "ext2 4k nodesize" 4096 mke2fs -b 4096
convert_test "ext3 4k nodesize" 4096 mke2fs -j -b 4096
convert_test "ext4 4k nodesize" 4096 mke2fs -t ext4 -b 4096
convert_test "ext2 8k nodesize" 8192 mke2fs -b 4096
convert_test "ext3 8k nodesize" 8192 mke2fs -j -b 4096
convert_test "ext4 8k nodesize" 8192 mke2fs -t ext4 -b 4096
convert_test "ext2 16k nodesize" 16384 mke2fs -b 4096
convert_test "ext3 16k nodesize" 16384 mke2fs -j -b 4096
convert_test "ext4 16k nodesize" 16384 mke2fs -t ext4 -b 4096
convert_test "ext2 32k nodesize" 32768 mke2fs -b 4096
convert_test "ext3 32k nodesize" 32768 mke2fs -j -b 4096
convert_test "ext4 32k nodesize" 32768 mke2fs -t ext4 -b 4096
convert_test "ext2 64k nodesize" 65536 mke2fs -b 4096
convert_test "ext3 64k nodesize" 65536 mke2fs -j -b 4096
convert_test "ext4 64k nodesize" 65536 mke2fs -t ext4 -b 4096
