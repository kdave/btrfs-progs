#!/bin/bash
# test btrfstune options that enable filesystem features

source $TOP/tests/common

check_prereq btrfs-debug-tree
check_prereq btrfs-show-super
check_prereq mkfs.btrfs
setup_root_helper

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

# test whether fsck can rebuild a corrupted extent tree
# parameters:
# - option for mkfs.btrfs -O, empty for defaults
# - option for btrfstune
# - string representing the feature in btrfs-show-super dump
test_feature()
{
	local mkfsfeatures
	local tuneopt
	local sbflag

	mkfsfeatures=${1:+-O ^$1}
	tuneopt="$2"
	sbflag="$3"

	run_check $SUDO_HELPER $TOP/mkfs.btrfs -f $mkfsfeatures $TEST_DEV
	if run_check_stdout $TOP/btrfs-show-super $TEST_DEV | \
			grep -q "$sbflag"; then
		_fail "FAIL: feature $sbflag must not be set on the base image"
	fi
	run_check $TOP/btrfstune $tuneopt $TEST_DEV
	if ! run_check_stdout $TOP/btrfs-show-super $TEST_DEV | \
			grep -q "$sbflag"; then
		_fail "FAIL: feature $sbflag not set"
	fi
	run_check $SUDO_HELPER $TOP/btrfs check $TEST_DEV
}

test_feature extref -r EXTENDED_IREF
test_feature skinny-metadata -x SKINNY_METADATA
test_feature no-holes -n NO_HOLES
test_feature '' '-S 1' SEEDING
