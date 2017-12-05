#!/bin/bash
# Test that any superblock is correctly detected
# and fixed by btrfs rescue

source "$TOP/tests/common"

check_prereq btrfs
check_prereq mkfs.btrfs
check_prereq btrfs-select-super

setup_root_helper

rm -f dev1
run_check truncate -s 260G dev1
loop=$(run_check_stdout $SUDO_HELPER losetup --find --show dev1)

# Create the test file system.
run_check $SUDO_HELPER "$TOP"/mkfs.btrfs -f "$loop"

function check_corruption {
	local sb_offset=$1
	local source_sb=$2


	# First we ensure we can mount it successfully
	run_check $SUDO_HELPER mount $loop "$TEST_MNT"
	run_check $SUDO_HELPER umount "$TEST_MNT"

	# Now corrupt 1k of the superblock at sb_offset
	run_check $SUDO_HELPER dd bs=1K count=1 seek=$(($sb_offset + 1)) if=/dev/zero of="$loop"

	#if corrupting one of the sb copies, copy it over the initial superblock
	if [ ! -z $source_sb ]; then
		local shift_val=$((16 << $source_sb * 12 ))
		run_check $SUDO_HELPER dd bs=1K count=4 seek=64 skip=$shift_val if="$loop" of="$loop"
	fi

	run_mustfail "Mounted fs with corrupted superblock" \
		$SUDO_HELPER mount $loop "$TEST_MNT"

	# Now run btrfs rescue which should fix the superblock. It uses 2
	# to signal success of recovery use mayfail to ignore that retval
	# but still log the output of the command
	run_mayfail $SUDO_HELPER "$TOP"/btrfs rescue super-recover -yv "$loop"
	if [ $? != 2 ]; then
		_fail "couldn't rescue super"
	fi

	run_check $SUDO_HELPER mount $loop "$TEST_MNT"
	run_check $SUDO_HELPER umount "$TEST_MNT"
}

_log "Corrupting first superblock"
check_corruption 64

_log "Corrupting second superblock"
check_corruption 65536 1

_log "Corrupting third superblock"
check_corruption 268435456 2

# Cleanup
run_check $SUDO_HELPER losetup -d "$loop"
rm -f dev1
