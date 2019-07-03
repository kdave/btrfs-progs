#!/bin/bash
# Test that any superblock is correctly detected and fixed by btrfs rescue

source "$TEST_TOP/common"

check_prereq btrfs
check_prereq mkfs.btrfs
check_prereq btrfs-select-super

setup_root_helper
prepare_test_dev 260G

run_check_mkfs_test_dev

check_corruption() {
	local sb_offset=$1
	local source_sb=$2

	# First we ensure we can mount it successfully
	run_check_mount_test_dev
	run_check_umount_test_dev

	# Now corrupt 1k of the superblock at sb_offset
	run_check $SUDO_HELPER dd bs=1K count=1 seek=$(($sb_offset + 1)) if=/dev/zero of="$TEST_DEV" conv=notrunc

	# if corrupting one of the sb copies, copy it over the initial superblock
	if [ ! -z "$source_sb" ]; then
		local shift_val=$((16 << $source_sb * 12 ))
		run_check $SUDO_HELPER dd bs=1K count=4 seek=64 skip="$shift_val" if="$TEST_DEV" of="$TEST_DEV" conv=notrunc
	fi

	# we can't use our mount helper, the following works for file image and
	# block device as TEST_DEV
	run_mustfail "mounted fs with corrupted superblock" \
		$SUDO_HELPER mount "$TEST_DEV" "$TEST_MNT"

	# Now run btrfs rescue which should fix the superblock. It uses 2
	# to signal success of recovery use mayfail to ignore that retval
	# but still log the output of the command
	run_mayfail $SUDO_HELPER "$TOP"/btrfs rescue super-recover -yv "$TEST_DEV"
	if [ $? != 2 ]; then
		_fail "couldn't rescue super"
	fi

	run_check_mount_test_dev
	run_check_umount_test_dev
}

# Corrupting first superblock
check_corruption 64

# Corrupting second superblock
check_corruption 65536 1

# Corrupting third superblock
check_corruption 268435456 2
