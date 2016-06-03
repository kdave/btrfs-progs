#!/bin/bash
#
# convert ext2/3/4 images to btrfs images, and make sure the results are
# clean.
#

unset TOP
unset LANG
LANG=C
SCRIPT_DIR=$(dirname $(readlink -f $0))
TOP=$(readlink -f $SCRIPT_DIR/../)
RESULTS="$TOP/tests/convert-tests-results.txt"

source $TOP/tests/common
source $TOP/tests/common.convert

# Allow child test to use $TOP and $RESULTS
export TOP
export RESULTS
export LANG

rm -f $RESULTS

setup_root_helper
prepare_test_dev 512M

run_one_test() {
	local testname

	testname="$1"
	echo "    [TEST/conv]   $testname"
	cd $testname
	echo "=== Entering $testname" >> $RESULTS
	if [ -x test.sh ]; then
		# Difference convert test case needs different tools to restore
		# and check image, so only support custom test scripts
		./test.sh
		if [ $? -ne 0 ]; then
			_fail "test failed for case $(basename $testname)"
		fi
	else
		_fail "custom test script not found"
	fi
}

if ! [ -z "$TEST" ]; then
	echo "    [TEST/conv]   skipped all convert tests, TEST=$TEST"
	exit 0
fi

for feature in '' 'extref' 'skinny-metadata' 'no-holes'; do
	convert_test "$feature" "ext4 4k nodesize" 4096 mke2fs -t ext4 -b 4096
	convert_test "$feature" "ext4 8k nodesize" 8192 mke2fs -t ext4 -b 4096
	convert_test "$feature" "ext4 16k nodesize" 16384 mke2fs -t ext4 -b 4096
	convert_test "$feature" "ext4 32k nodesize" 32768 mke2fs -t ext4 -b 4096
	convert_test "$feature" "ext4 64k nodesize" 65536 mke2fs -t ext4 -b 4096
done

# Test special images
for i in $(find $TOP/tests/convert-tests -maxdepth 1 -mindepth 1 -type d \
	   ${TEST:+-name "$TEST"} | sort)
do
	run_one_test "$i"
done
