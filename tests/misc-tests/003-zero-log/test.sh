#!/bin/bash
# test zero-log

source "$TEST_TOP/common"

check_prereq mkfs.btrfs
check_prereq btrfs
prepare_test_dev

get_log_root()
{
	"$TOP/btrfs" inspect-internal dump-super "$1" | \
		grep '^log_root\>' | awk '{print $2}'
}
get_log_root_level() {
	"$TOP/btrfs" inspect-internal dump-super "$1" | \
		grep '^log_root_level' | awk '{print $2}'
}

test_zero_log()
{
	# FIXME: we need an image with existing log_root
	run_check_mkfs_test_dev --rootdir "$INTERNAL_BIN/Documentation"
	run_check "$TOP/btrfs" inspect-internal dump-super "$TEST_DEV"
	run_check "$TOP/btrfs" rescue zero-log "$TEST_DEV"
	log_root=$(get_log_root "$TEST_DEV")
	log_root_level=$(get_log_root "$TEST_DEV")
	if [ "$log_root" != 0 ]; then
		_fail "FAIL: log_root not reset"
	fi
	if [ "$log_root_level" != 0 ]; then
		_fail "FAIL: log_root_level not reset"
	fi
	run_check "$TOP/btrfs" inspect-internal dump-super "$TEST_DEV"
	run_check $SUDO_HELPER "$TOP/btrfs" check "$TEST_DEV"
}

test_zero_log standalone
test_zero_log internal
