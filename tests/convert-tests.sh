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

test(){
	echo "    [TEST]   $1"
	shift
	echo "creating ext image with: $*" >> convert-tests-results.txt
	# 256MB is the smallest acceptable btrfs image.
	rm -f $here/test.img >> convert-tests-results.txt 2>&1 \
		|| _fail "could not remove test image file"
	truncate -s 256M $here/test.img >> convert-tests-results.txt 2>&1 \
		|| _fail "could not create test image file"
	$* -F $here/test.img >> convert-tests-results.txt 2>&1 \
		|| _fail "filesystem create failed"
	$here/btrfs-convert $here/test.img >> convert-tests-results.txt 2>&1 \
		|| _fail "btrfs-convert failed"
	$here/btrfs check $here/test.img >> convert-tests-results.txt 2>&1 \
		|| _fail "btrfs check detected errors"
}

# btrfs-convert requires 4k blocksize.
test "ext2" mke2fs -b 4096
test "ext3" mke2fs -j -b 4096
test "ext4" mke2fs -t ext4 -b 4096
