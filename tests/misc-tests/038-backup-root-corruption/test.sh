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
	run_check_stdout $SUDO_HELPER "$TOP/btrfs" inspect-internal dump-super -f "$TEST_DEV"
}

# Ensure currently active backup slot is the expected one (slot 3)
backup2_root_ptr=$(dump_super | grep -A1 "backup 2" | grep backup_tree_root | awk '{print $2}')

main_root_ptr=$(dump_super | grep root | head -n1 | awk '{print $2}')

[ "$backup2_root_ptr" -eq "$main_root_ptr" ] || _fail "Backup slot 2 is not in use"

run_check "$TOP/btrfs-corrupt-block" -m $main_root_ptr -f generation "$TEST_DEV"

# Should fail because the root is corrupted
run_mustfail "Unexpected successful mount" \
	$SUDO_HELPER mount "$TEST_DEV" "$TEST_MNT"

# Cycle mount with the backup to force rewrite of slot 3
run_check_mount_test_dev -o usebackuproot
run_check_umount_test_dev

# Since we've used backup 1 as the usable root, then backup 2 should have been
# overwritten
main_root_ptr=$(dump_super | grep root | head -n1 | awk '{print $2}')
backup2_new_root_ptr=$(dump_super | grep -A1 "backup 2" | grep backup_tree_root | awk '{print $2}')

[ "$backup2_root_ptr" -ne "$backup2_new_root_ptr" ] || _fail "Backup 2 not overwritten"
