#!/bin/bash
# test convert-thread-conflict

source $TOP/tests/common

check_prereq btrfs-convert

mkfs.ext4 -V &>/dev/null || _not_run "mkfs.ext4 not found"
prepare_test_dev 1G

for ((i = 0; i < 20; i++)); do
	echo "loop $i" >>$RESULTS
	mkfs.ext4 -F "$TEST_DEV" &>>$RESULTS || _not_run "mkfs.ext4 failed"
	run_check $TOP/btrfs-convert "$TEST_DEV"
done
