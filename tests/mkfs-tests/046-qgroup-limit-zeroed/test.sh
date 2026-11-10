#!/bin/bash
# Verify that mkfs -O quota zeroes all members of the QGROUP_LIMIT items.

source "$TEST_TOP/common" || exit

check_prereq mkfs.btrfs
check_prereq btrfs

setup_root_helper
prepare_test_dev

run_check $SUDO_HELPER "$TOP/mkfs.btrfs" -f -O quota "$TEST_DEV"

# A good limit item should looks like:
#
# item 2 key (0 QGROUP_LIMIT 0/5) itemoff 16027 itemsize 40
#         flags 0
#         max_referenced 0 max_exclusive 0
#         rsv_referenced 0 rsv_exclusive 0
output=$(run_check_stdout $SUDO_HELPER "$TOP/btrfs" inspect-internal dump-tree -t quota "$TEST_DEV" | \
	grep -A 3 "QGROUP_LIMIT")

if [ -z "$output" ]; then
	_fail "no QGROUP_LIMIT item found"
fi

for member in flags max_referenced max_exclusive rsv_referenced rsv_exclusive; do
	echo "$output" | grep -q -w "$member 0" ||
		_fail "QGROUP_LIMIT member $member is not zero"
done
