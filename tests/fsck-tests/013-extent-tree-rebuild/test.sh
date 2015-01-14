#!/bin/bash

source $top/tests/common

if [ -z $TEST_DEV ]; then
	echo "    [NOTRUN] extent tree rebuild, need TEST_DEV variant"
	exit 0
fi

if [ -z $TEST_MNT ];then
	echo "    [NOTRUN] extent tree rebuild, need TEST_MNT variant"
	exit 0
fi

# test whether fsck can rebuild a corrupted extent tree
test_extent_tree_rebuild()
{
	echo "     [TEST]    extent tree rebuild"
	$top/mkfs.btrfs -f $TEST_DEV >> /dev/null 2>&1 || _fail "fail to mkfs"

	run_check mount $TEST_DEV $TEST_MNT
	cp -aR /lib/modules/`uname -r`/ $TEST_MNT 2>&1

	for i in `seq 1 100`;do
		$top/btrfs sub snapshot $TEST_MNT \
			$TEST_MNT/snapaaaaaaa_$i >& /dev/null
	done
	run_check umount $TEST_DEV

	# get extent root bytenr
	extent_root_bytenr=`$top/btrfs-debug-tree -r $TEST_DEV | \
			    grep extent | awk '{print $7}'`
	if [ -z $extent_root_bytenr ];then
		_fail "fail to get extent root bytenr"
	fi

	# corrupt extent root node block
	run_check $top/btrfs-corrupt-block -l $extent_root_bytenr \
		-b 4096 $TEST_DEV

	$top/btrfs check $TEST_DEV >& /dev/null && \
			_fail "btrfs check should detect failure"
	run_check $top/btrfs check --init-extent-tree $TEST_DEV
	run_check $top/btrfs check $TEST_DEV
}

test_extent_tree_rebuild
