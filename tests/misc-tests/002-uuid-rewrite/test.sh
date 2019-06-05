#!/bin/bash
# test btrfstune uuid rewriting options

source "$TEST_TOP/common"

check_prereq mkfs.btrfs
check_prereq btrfstune
check_prereq btrfs

prepare_test_dev

get_fs_uuid() {
	run_check_stdout "$TOP/btrfs" inspect-internal dump-super "$1" | \
		grep '^fsid' | awk '{print $2}'
}

test_uuid_random()
{
	local origuuid

	origuuid=11111111-a101-4031-b29a-379d4f8b7a2d

	run_check_mkfs_test_dev    \
		--uuid "$origuuid" \
		--rootdir "$INTERNAL_BIN/Documentation"
	run_check "$TOP/btrfs" inspect-internal dump-super "$TEST_DEV"
	currentfsid=$(run_check_stdout "$TOP/btrfstune" -f -u "$TEST_DEV" | \
		grep -i 'current fsid:' | awk '{print $3}')
	if ! [ "$currentfsid" = "$origuuid" ]; then
		_fail "FAIL: current UUID mismatch"
	fi
	run_check "$TOP/btrfs" inspect-internal dump-super "$TEST_DEV"
	run_check $SUDO_HELPER "$TOP/btrfs" check "$TEST_DEV"
}

test_uuid_user()
{
	local origuuid
	local newuuid

	origuuid=22222222-d324-4f92-80e9-7658bf3b845f
	newuuid=33333333-bfc9-4045-9399-a396dc6893b3

	run_check_mkfs_test_dev    \
		--uuid "$origuuid" \
		--rootdir "$INTERNAL_BIN/Documentation"
	run_check "$TOP/btrfs" inspect-internal dump-super "$TEST_DEV"
	run_check "$TOP/btrfstune" -f -U "$newuuid" \
		"$TEST_DEV"
	# btrfs inspect-internal dump-super is called within get_fs_uuid
	fsid=$(get_fs_uuid "$TEST_DEV")
	if ! [ "$fsid" = "$newuuid" ]; then
		_fail "FAIL: UUID not rewritten"
	fi
	run_check $SUDO_HELPER "$TOP/btrfs" check "$TEST_DEV"
}

test_uuid_random
test_uuid_user
