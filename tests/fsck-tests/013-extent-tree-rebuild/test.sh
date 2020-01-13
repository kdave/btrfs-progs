#!/bin/bash

source "$TEST_TOP/common"

check_prereq btrfs-corrupt-block
check_prereq mkfs.btrfs
check_prereq btrfs

setup_root_helper
prepare_test_dev

# test whether fsck can rebuild a corrupted extent tree
test_extent_tree_rebuild()
{
	run_check_mkfs_test_dev
	run_check_mount_test_dev
	generate_dataset small

	for i in `seq 1 100`;do
		run_check $SUDO_HELPER "$TOP/btrfs" sub snapshot "$TEST_MNT" \
			"$TEST_MNT/snapaaaaaaa_$i"
	done
	run_check_umount_test_dev

	# get extent root bytenr
	extent_root_bytenr=`$SUDO_HELPER "$TOP/btrfs" inspect-internal dump-tree -r "$TEST_DEV" | \
			    grep extent | awk '{print $7}'`
	if [ -z "$extent_root_bytenr" ];then
		_fail "fail to get extent root bytenr"
	fi

	# corrupt extent root node block
	run_check $SUDO_HELPER "$INTERNAL_BIN/btrfs-corrupt-block" -l "$extent_root_bytenr" \
		-b 4096 "$TEST_DEV"

	$SUDO_HELPER "$TOP/btrfs" check "$TEST_DEV" >& /dev/null && \
			_fail "btrfs check should detect failure"
	run_check $SUDO_HELPER "$TOP/btrfs" check --repair --force --init-extent-tree "$TEST_DEV"
	run_check $SUDO_HELPER "$TOP/btrfs" check "$TEST_DEV"
}

test_extent_tree_rebuild
