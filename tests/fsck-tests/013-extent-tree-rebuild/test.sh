#!/bin/bash

source $TOP/tests/common

check_prereq btrfs-debug-tree
setup_root_helper

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
	run_check $SUDO_HELPER $TOP/mkfs.btrfs -f $TEST_DEV

	run_check $SUDO_HELPER mount $TEST_DEV $TEST_MNT
	run_check $SUDO_HELPER cp -aR /lib/modules/`uname -r`/ $TEST_MNT

	for i in `seq 1 100`;do
		run_check $SUDO_HELPER $TOP/btrfs sub snapshot $TEST_MNT \
			$TEST_MNT/snapaaaaaaa_$i
	done
	run_check $SUDO_HELPER umount $TEST_DEV

	# get extent root bytenr
	extent_root_bytenr=`$SUDO_HELPER $TOP/btrfs-debug-tree -r $TEST_DEV | \
			    grep extent | awk '{print $7}'`
	if [ -z $extent_root_bytenr ];then
		_fail "fail to get extent root bytenr"
	fi

	# corrupt extent root node block
	run_check $SUDO_HELPER $TOP/btrfs-corrupt-block -l $extent_root_bytenr \
		-b 4096 $TEST_DEV

	$SUDO_HELPER $TOP/btrfs check $TEST_DEV >& /dev/null && \
			_fail "btrfs check should detect failure"
	run_check $SUDO_HELPER $TOP/btrfs check --init-extent-tree $TEST_DEV
	run_check $SUDO_HELPER $TOP/btrfs check $TEST_DEV
}

test_extent_tree_rebuild
