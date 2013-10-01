#!/bin/bash
#
# loop through all of our bad images and make sure fsck repairs them properly
#
# It's GPL, same as everything else in this tree.
#

here=`pwd`

_fail()
{
	echo "$*" | tee -a fsck-tests-results.txt
	exit 1
}

rm -f fsck-tests-results.txt

for i in $(find $here/tests/fsck-tests -name '*.img')
do
	echo "     [TEST]    $(basename $i)"
	echo "testing image $i" >> fsck-tests-results.txt
	$here/btrfs-image -r $i test.img >> fsck-tests-results.txt 2>&1 \
		|| _fail "restore failed"
	$here/btrfsck test.img >> fsck-test-results.txt 2>&1
	[ $? -eq 0 ] && _fail "btrfsck should have detected corruption"

	$here/btrfsck --repair test.img >> fsck-test-results.txt 2>&1 || \
		_fail "btrfsck should have repaired the image"

	$here/btrfsck test.img >> fsck-test-results.txt 2>&1 || \
		_fail "btrfsck did not correct corruption"
done
