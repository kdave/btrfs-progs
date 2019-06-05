#!/bin/bash
# test for "subvolume get-default/set-default"

check_default_id()
{
	id=$(run_check_stdout $SUDO_HELPER "$TOP/btrfs" subvolume get-default .) \
		|| { echo "$id"; exit 1; }
	if $(echo "$id" | grep -vq "ID $1"); then
		_fail "subvolume get-default: default id is not $1, but $id"
	fi
}

source "$TEST_TOP/common"

check_prereq mkfs.btrfs
check_prereq btrfs

setup_root_helper
prepare_test_dev

run_check_mkfs_test_dev
run_check_mount_test_dev
cd "$TEST_MNT"

check_default_id 5

# check "subvol set-default <subvolid> <path>"
run_check $SUDO_HELPER "$TOP/btrfs" subvolume create sub
id=$(run_check_stdout "$TOP/btrfs" inspect-internal rootid sub)
run_check $SUDO_HELPER "$TOP/btrfs" subvolume set-default "$id" .
check_default_id "$id"

run_mustfail "set-default to non existent id" \
	$SUDO_HELPER "$TOP/btrfs" subvolume set-default 100 .

# check "subvol set-default <subvolume>"
run_check $SUDO_HELPER "$TOP/btrfs" subvolume create sub2
id=$(run_check_stdout "$TOP/btrfs" inspect-internal rootid sub2)
run_check $SUDO_HELPER "$TOP/btrfs" subvolume set-default ./sub2
check_default_id "$id"

run_check $SUDO_HELPER mkdir sub2/dir
run_mustfail "set-default to normal directory" \
	$SUDO_HELPER "$TOP/btrfs" subvolume set-default ./sub2/dir

cd ..
run_check_umount_test_dev
