#!/bin/bash
# Test that a corrupted filesystem will correctly handle writing of backup root

source "$TEST_TOP/common"

check_prereq mkfs.btrfs
check_prereq btrfs
check_prereq btrfs-corrupt-block

setup_root_helper
prepare_test_dev

# Create a file and unmount to commit some backup roots
run_check_mkfs_test_dev
run_check_mount_test_dev
run_check $SUDO_HELPER touch "$TEST_MNT/file"
run_check_umount_test_dev

dump_super() {
	# In this test, we will dump super block multiple times, while the
	# existing run_check*() helpers will always dump all the output into
	# the log, flooding the log and hiding the important info.
	# Thus here we call "btrfs" directly.
	$SUDO_HELPER "$TOP/btrfs" inspect-internal dump-super -f "$TEST_DEV"
}

main_root_ptr=$(dump_super | awk '/^root\t/{print $2}')
# Grab current fs generation, and it will be used to determine which backup
# slot to use
cur_gen=$(dump_super | grep ^generation | awk '{print $2}')
backup_gen=$(($cur_gen - 1))

# Grab the slot which matches @backup_gen
found=$(dump_super | grep backup_tree_root | grep -n "gen: $backup_gen")

if [ -z "$found" ]; then
	_fail "Unable to find a backup slot with generation $backup_gen"
fi

slot_num=$(echo $found | cut -f1 -d:)
# To follow the dump-super output, where backup slot starts at 0.
slot_num=$(($slot_num - 1))

# Save the backup slot info into the log
_log "Backup slot $slot_num will be utilized"
dump_super | run_check grep -A9 "backup $slot_num:"

run_check "$INTERNAL_BIN/btrfs-corrupt-block" -m "$main_root_ptr" -f generation "$TEST_DEV"

# Should fail because the root is corrupted
run_mustfail "Unexpected successful mount" \
	$SUDO_HELPER mount "$TEST_DEV" "$TEST_MNT"

# Cycle mount with the backup to force rewrite of slot 3
run_check_mount_test_dev -o usebackuproot
run_check_umount_test_dev

main_root_ptr=$(dump_super | awk '/^root\t/{print $2}')

# The next slot should be overwritten
slot_num=$(( ($slot_num + 1) % 4 ))
backup_new_root_ptr=$(dump_super | grep -A1 "backup $slot_num" | grep backup_tree_root | awk '{print $2}')

[ "$main_root_ptr" -ne "$backup_new_root_ptr" ] || _fail "Backup 2 not overwritten"
