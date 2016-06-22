#!/bin/bash
#
# convert ext2/3/4 images to btrfs images, and make sure the results are
# clean.

LANG=C
SCRIPT_DIR=$(dirname $(readlink -f $0))
TOP=$(readlink -f $SCRIPT_DIR/../)
TEST_DEV=${TEST_DEV:-}
RESULTS="$TOP/tests/convert-tests-results.txt"
IMAGE="$TOP/tests/test.img"

source $TOP/tests/common
source $TOP/tests/common.convert

export TOP
export RESULTS
export LANG
export IMAGE
export TEST_DEV

rm -f $RESULTS

run_one_test() {
	local testdir
	local testname

	testdir="$1"
	testname=$(basename "$testdir")
	echo "    [TEST/conv]   $testname"
	cd "$testdir"
	echo "=== Entering $testname" >> $RESULTS
	if [ -x test.sh ]; then
		# Only support custom test scripts
		./test.sh
		if [ $? -ne 0 ]; then
			_fail "test failed for case $testname"
		fi
	else
		_fail "custom test script not found"
	fi
}

# Test special images
for i in $(find $TOP/tests/convert-tests -maxdepth 1 -mindepth 1 -type d \
	   ${TEST:+-name "$TEST"} | sort)
do
	run_one_test "$i"
done
