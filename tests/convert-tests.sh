#!/bin/bash
#
# convert ext2/3/4 images to btrfs images, and make sure the results are
# clean.

LANG=C
SCRIPT_DIR=$(dirname $(readlink -f "$0"))
if [ -z "$TOP" ]; then
	TOP=$(readlink -f "$SCRIPT_DIR/../")
	if [ -f "$TOP/configure.ac" ]; then
		# inside git
		TEST_TOP="$TOP/tests"
		INTERNAL_BIN="$TOP"
	else
		# external, defaults to system binaries
		TOP=$(dirname `type -p btrfs`)
		TEST_TOP="$SCRIPT_DIR"
		INTERNAL_BIN="$TEST_TOP"
	fi
else
	# assume external, TOP set from commandline
	TEST_TOP="$SCRIPT_DIR"
	INTERNAL_BIN="$TEST_TOP"
fi
if ! [ -x "$TOP/btrfs" ]; then
	echo "ERROR: cannot execute btrfs from TOP=$TOP"
	exit 1
fi
TEST_DEV=${TEST_DEV:-}
RESULTS="$TEST_TOP/convert-tests-results.txt"
IMAGE="$TEST_TOP/test.img"

source "$TEST_TOP/common"
source "$TEST_TOP/common.convert"

export INTERNAL_BIN
export TEST_TOP
export TOP
export RESULTS
export LANG
export IMAGE
export TEST_DEV

rm -f "$RESULTS"

check_kernel_support
check_kernel_support_reiserfs
# anything expected by common.convert
check_global_prereq getfacl
check_global_prereq setfacl
check_global_prereq md5sum

run_one_test() {
	local testdir
	local testname

	testdir="$1"
	testname=$(basename "$testdir")
	echo "    [TEST/conv]   $testname"
	cd "$testdir"
	echo "=== START TEST $testname" >> "$RESULTS"
	if [ -x test.sh ]; then
		# Only support custom test scripts
		./test.sh
		if [ $? -ne 0 ]; then
			if [[ $TEST_LOG =~ dump ]]; then
				# the logs can be large and may exceed the
				# limits, use 4MB for now
				tail -c 3900000 "$RESULTS"
			fi
			_fail "test failed for case $testname"
		fi
		check_test_results "$RESULTS" "$testname"
	else
		_fail "custom test script not found"
	fi
}

# Test special images
for i in $(find "$TEST_TOP/convert-tests" -maxdepth 1 -mindepth 1 -type d \
	   ${TEST:+-name "$TEST"} | sort)
do
	run_one_test "$i"
done
