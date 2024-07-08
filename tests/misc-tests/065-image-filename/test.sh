#!/bin/bash
# Verify "btrfs-image -s" sanitizes the filenames correctly

source "$TEST_TOP/common" || exit
source "$TEST_TOP/common.convert" || exit

setup_root_helper
prepare_test_dev

declare -a filenames=("%%top_secret%%" "@@secret@@" "||confidential||")
tmp=$(_mktemp "image-filename")

run_check_mkfs_test_dev
run_check_mount_test_dev
for i in ${filenames[@]}; do
	run_check $SUDO_HELPER touch "$TEST_MNT/$i"
done
run_check_umount_test_dev

run_check "$TOP/btrfs-image" "$TEST_DEV" "$tmp"
_log "strings found inside the regular dump:"
strings "$tmp" >> "$RESULTS"
for i in ${filenames[@]}; do
	if ! grep -q "$i" "$tmp"; then
		rm -f -- "$tmp"
		_fail "regular dump sanitized the filenames"
	fi
done
run_check "$TOP/btrfs-image" -s "$TEST_DEV" "$tmp"
_log "strings found inside the sanitize dump:"
strings "$tmp" >> "$RESULTS"
for i in ${filenames[@]}; do
	if grep -q "$i" "$tmp"; then
		rm -f -- "$tmp"
		_fail "filenames not properly sanitized"
	fi
done
rm -f -- "$tmp"
