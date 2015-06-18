#!/bin/bash
# test btrfstune uuid rewriting options

source $TOP/tests/common

check_prereq btrfs-debug-tree
check_prereq btrfs-show-super
check_prereq mkfs.btrfs
check_prereq btrfstune

if [ -z $TEST_DEV ]; then
	echo "\$TEST_DEV not given, use $TOP/test/test.img as fallback" >> \
		$RESULTS
	TEST_DEV="$TOP/tests/test.img"

	# Need at least 1G to avoid mixed block group, which extent tree
	# rebuild doesn't support.
	run_check truncate -s 1G $TEST_DEV
fi

if [ -z $TEST_MNT ];then
	echo "    [NOTRUN] extent tree rebuild, need TEST_MNT variant"
	exit 0
fi

get_fs_uuid() {
	local image

	image="$1"
	run_check_stdout $TOP/btrfs-show-super "$image" | \
		grep '^fsid' | awk '{print $2}'
}

test_uuid_random()
{
	local origuuid

	origuuid=11111111-a101-4031-b29a-379d4f8b7a2d

	run_check $SUDO_HELPER $TOP/mkfs.btrfs -f \
		--uuid $origuuid \
		--rootdir $TOP/Documentation \
		$TEST_DEV
	run_check $TOP/btrfs-show-super "$TEST_DEV"
	currentfsid=$(run_check_stdout $TOP/btrfstune -f -u $TEST_DEV | \
		grep -i 'current fsid:' | awk '{print $3}')
	if ! [ $currentfsid = $origuuid ]; then
		_fail "FAIL: current UUID mismatch"
	fi
	run_check $TOP/btrfs-show-super "$TEST_DEV"
	run_check $SUDO_HELPER $TOP/btrfs check $TEST_DEV
}

test_uuid_user()
{
	local origuuid
	local newuuid

	origuuid=22222222-d324-4f92-80e9-7658bf3b845f
	newuuid=33333333-bfc9-4045-9399-a396dc6893b3

	run_check $SUDO_HELPER $TOP/mkfs.btrfs -f \
		--uuid $origuuid \
		--rootdir $TOP/Documentation \
		$TEST_DEV
	run_check $TOP/btrfs-show-super "$TEST_DEV"
	run_check $TOP/btrfstune -f -U $newuuid \
		$TEST_DEV
	# btrfs-show-super is called within get_fs_uuid
	fsid=$(get_fs_uuid $TEST_DEV)
	if ! [ $fsid = $newuuid ]; then
		_fail "FAIL: UUID not rewritten"
	fi
	run_check $SUDO_HELPER $TOP/btrfs check $TEST_DEV
}

test_uuid_random
test_uuid_user
