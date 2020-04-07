#!/bin/bash
#
# test commands of inspect-internal rootid

source "$TEST_TOP/common"

check_prereq mkfs.btrfs
check_prereq btrfs

prepare_test_dev

run_check_mkfs_test_dev
run_check_mount_test_dev
run_check $SUDO_HELPER chmod a+rw "$TEST_MNT"
cd "$TEST_MNT"

run_check "$TOP/btrfs" subvolume create sub
run_check "$TOP/btrfs" subvolume create sub/subsub
run_check mkdir dir
run_check touch file1
run_check touch dir/file2
run_check touch sub/file3

id1=$(_get_subvolid .) || { echo $id1; exit 1; }
id2=$(_get_subvolid sub) || { echo $id2; exit 1; }
id3=$(_get_subvolid sub/subsub) || { echo $id3; exit 1; }
id4=$(_get_subvolid dir) || { echo $id4; exit 1; }
id5=$(_get_subvolid file1) || { echo $id5; exit 1; }
id6=$(_get_subvolid dir/file2) || { echo $id6; exit 1; }
id7=$(_get_subvolid sub/file3) || { echo $id7; exit 1; }

if ! ([ "$id1" -ne "$id2" ] && [ "$id1" -ne "$id3" ] && [ "$id2" -ne "$id3" ]); then
	_fail "inspect-internal rootid: each subvolume must have different id"
fi

if ! ([ "$id1" -eq "$id4" ] && [ "$id1" -eq "$id5" ] && [ "$id1" -eq "$id6" ]); then
	_fail "inspect-internal rootid: rootid mismatch found"
fi

if ! ([ "$id2" -eq "$id7" ]); then
	_fail "inspect-internal rootid: rootid mismatch found"
fi

run_mustfail "should fail for non existent file" \
	"$TOP/btrfs" inspect-internal rootid no_such_file
run_mustfail "should fail for non-btrfs filesystem" \
	"$TOP/btrfs" inspect-internal rootid /dev/null

cd ..
run_check_umount_test_dev
