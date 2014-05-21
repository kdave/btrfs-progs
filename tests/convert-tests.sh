#!/bin/bash
#
# convert ext2/3/4 images to btrfs images, and make sure the results are
# clean.
#

here=`pwd`

_fail()
{
	echo "$*" | tee -a convert-tests-results.txt
	exit 1
}

rm -f convert-tests-results.txt
rm -f test.img

test(){
	echo "     [TEST]    $1"
        shift
        echo "creating ext image with: $*" >> convert-tests-results.txt
	# 256MB is the smallest acceptable btrfs image.
	dd if=/dev/zero of=$here/test.img bs=1024 count=$((256*1024)) \
		>> convert-tests-results.txt 2>&1 || _fail "dd failed"
	$* -F $here/test.img >> convert-tests-results.txt 2>&1 \
		|| _fail "filesystem create failed"
	$here/btrfs-convert $here/test.img >> convert-tests-results.txt 2>&1 \
		|| _fail "btrfs-convert failed"
	$here/btrfsck $here/test.img >> convert-tests-results.txt 2>&1 \
		|| _fail "btrfsck detected errors"
}

test "ext2, 4k blocksize" mke2fs -b 4096
test "ext3, 4k blocksize" mke2fs -j -b 4096
test "ext4, 4k blocksize" mke2fs -t ext4 -b 4096
