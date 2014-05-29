#!/bin/bash
#
# loop through all of our bad images and make sure fsck repairs them properly
#
# It's GPL, same as everything else in this tree.
#

here=`pwd`
TEST_DEV=
TEST_MNT=
RESULT="fsck-tests-results.txt"

_fail()
{
	echo "$*" | tee -a $RESULT
	exit 1
}

run_check()
{
	echo "############### $@" >> $RESULT 2>&1
	"$@" >> $RESULT 2>&1 || _fail "failed: $@"
}

rm -f $RESULT

if [ -z $TEST_DEV ] || [ -z $TEST_MNT ];then
	_fail "please set TEST_DEV and TEST_MNT"
fi

# test rely on corrupting blocks tool
run_check make btrfs-corrupt-block

for i in $(find $here/tests/fsck-tests -name '*.img')
do
	echo "     [TEST]    $(basename $i)"
	echo "testing image $i" >> $RESULT

	run_check $here/btrfs-image -r $i test.img

	$here/btrfsck test.img >> $RESULT 2>&1
	[ $? -eq 0 ] && _fail "btrfsck should have detected corruption"

	run_check $here/btrfsck --repair test.img
	run_check $here/btrfsck test.img
done

# test whether fsck can rebuild a corrupted extent tree
test_extent_tree_rebuild()
{
	echo "     [TEST]    extent tree rebuild"
	$here/mkfs.btrfs -f $TEST_DEV >> /dev/null 2>&1 || _fail "fail to mkfs"

	run_check mount $TEST_DEV $TEST_MNT
	cp -aR /lib/modules/`uname -r`/ $TEST_MNT 2>&1

	for i in `seq 1 100`;do
		$here/btrfs sub snapshot $TEST_MNT \
			$TEST_MNT/snapaaaaaaa_$i >& /dev/null
	done
	run_check umount $TEST_DEV

	# get extent root bytenr
	extent_root_bytenr=`$here/btrfs-debug-tree -r $TEST_DEV | grep extent | awk '{print $7}'`
	if [ -z $extent_root_bytenr ];then
		_fail "fail to get extent root bytenr"
	fi

	# corrupt extent root node block
	run_check $here/btrfs-corrupt-block -l $extent_root_bytenr \
		-b 4096 $TEST_DEV

	$here/btrfs check $TEST_DEV >& /dev/null && \
			_fail "fsck should detect failure"
	run_check $here/btrfs check --init-extent-tree $TEST_DEV
	run_check $here/btrfs check $TEST_DEV
}

test_extent_tree_rebuild
