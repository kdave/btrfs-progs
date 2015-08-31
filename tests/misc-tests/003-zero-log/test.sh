#!/bin/bash
# test zero-log

source $TOP/tests/common

check_prereq btrfs-show-super
check_prereq mkfs.btrfs
check_prereq btrfs
prepare_test_dev

get_log_root()
{
	local image

	image="$1"
	$TOP/btrfs-show-super "$image" | \
		grep '^log_root\>' | awk '{print $2}'
}
get_log_root_level() {
	local image

	image="$1"
	$TOP/btrfs-show-super "$image" | \
		grep '^log_root_level' | awk '{print $2}'
}

test_zero_log()
{
	# FIXME: we need an image with existing log_root
	run_check $SUDO_HELPER $TOP/mkfs.btrfs -f \
		--rootdir $TOP/Documentation \
		$TEST_DEV
	run_check $TOP/btrfs-show-super $TEST_DEV
	if [ "$1" = 'standalone' ]; then
		run_check $TOP/btrfs rescue zero-log $TEST_DEV
	else
		run_check $TOP/btrfs-zero-log $TEST_DEV
	fi
	log_root=$(get_log_root $TEST_DEV)
	log_root_level=$(get_log_root $TEST_DEV)
	if [ "$log_root" != 0 ]; then
		_fail "FAIL: log_root not reset"
	fi
	if [ "$log_root_level" != 0 ]; then
		_fail "FAIL: log_root_level not reset"
	fi
	run_check $TOP/btrfs-show-super $TEST_DEV
	run_check $SUDO_HELPER $TOP/btrfs check $TEST_DEV
}

test_zero_log standalone
test_zero_log internal
