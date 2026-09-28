#!/bin/bash
# Test that both -M and the FS feature mixed-bg create a FS with the same
# data and metadata profiles.

source "$TEST_TOP/common" || exit

check_prereq mkfs.btrfs

setup_root_helper
prepare_test_dev

check_mixed() {
	local output
	output=$(run_check_stdout "$TOP/mkfs.btrfs" "$@" -f "$TEST_DEV" | grep -E 'Data|Metadata')

	if echo "$output" | grep -q -v 'Data+Metadata:'; then
		_fail "created a mixed-bg filesystem with differing profiles using '$*' options"
	fi
}

check_mixed -M
check_mixed -O mixed-bg
