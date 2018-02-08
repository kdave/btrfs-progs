#!/bin/bash
#
# command line interface coverage tests

LANG=C
SCRIPT_DIR=$(dirname $(readlink -f "$0"))
TEST_TOP=$(readlink -f "$SCRIPT_DIR/../tests/")
TOP=$(readlink -f "$SCRIPT_DIR/../")
if ! [ -f "$TOP/btrfs" ];then
	TOP=$(dirname `which btrfs`)
fi
TEST_DEV=${TEST_DEV:-}
RESULTS="$TEST_TOP/cli-tests-results.txt"
IMAGE="$TEST_TOP/test.img"

source "$TEST_TOP/common"

export TEST_TOP
export TOP
export RESULTS
export LANG
export IMAGE
export TEST_DEV

rm -f "$RESULTS"

check_prereq btrfs
check_kernel_support

# The tests are driven by their custom script called 'test.sh'

for i in $(find "$TEST_TOP/cli-tests" -maxdepth 1 -mindepth 1 -type d	\
	${TEST:+-name "$TEST"} | sort)
do
	name=$(basename "$i")
	cd "$i"
	if [ -x test.sh ]; then
		echo "=== Entering $i" >> "$RESULTS"
		echo "    [TEST/cli]   $name"
		./test.sh
		if [ $? -ne 0 ]; then
			if [[ $TEST_LOG =~ dump ]]; then
				cat "$RESULTS"
			fi
			_fail "test failed for case $(basename $i)"
		fi
	fi
	cd "$TEST_TOP"
done
