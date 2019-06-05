#!/bin/bash
# test btrfstune options that enable filesystem features

source "$TEST_TOP/common"

check_prereq mkfs.btrfs
check_prereq btrfstune
check_prereq btrfs

setup_root_helper
prepare_test_dev

# test whether fsck can rebuild a corrupted extent tree
# parameters:
# - option for mkfs.btrfs -O, empty for defaults
# - option for btrfstune
# - string representing the feature in dump-super output
test_feature()
{
	local mkfsfeatures
	local tuneopt
	local sbflag

	mkfsfeatures=$1
	tuneopt="$2"
	sbflag="$3"

	run_check_mkfs_test_dev ${mkfsfeatures:+-O ^"$mkfsfeatures"}
	if run_check_stdout "$TOP/btrfs" inspect-internal dump-super "$TEST_DEV" | \
			grep -q "$sbflag"; then
		_fail "FAIL: feature $sbflag must not be set on the base image"
	fi
	run_check "$TOP/btrfstune" "$tuneopt" "$TEST_DEV"
	if ! run_check_stdout "$TOP/btrfs" inspect-internal dump-super "$TEST_DEV" | \
			grep -q "$sbflag"; then
		_fail "FAIL: feature $sbflag not set"
	fi
	run_check $SUDO_HELPER "$TOP/btrfs" check "$TEST_DEV"
}

test_feature extref -r EXTENDED_IREF
test_feature skinny-metadata -x SKINNY_METADATA
test_feature no-holes -n NO_HOLES
test_feature '' '-S 1' SEEDING
