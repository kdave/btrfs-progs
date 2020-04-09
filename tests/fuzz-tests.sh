#!/bin/bash
#
# misc tests on fuzzed or crafted images

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
		TOP=$(dirname `which btrfs`)
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
RESULTS="$TEST_TOP/fuzz-tests-results.txt"
IMAGE="$TEST_TOP/test.img"

source "$TEST_TOP/common"

export INTERNAL_BIN
export TEST_TOP
export TOP
export RESULTS
export LANG
export IMAGE
export TEST_DEV

rm -f "$RESULTS"

check_prereq btrfs

# The tests are driven by their custom script called 'test.sh'

for i in $(find "$TEST_TOP/fuzz-tests" -maxdepth 1 -mindepth 1 -type d	\
	${TEST:+-name "$TEST"} | sort)
do
	name=$(basename "$i")
	cd $i
	if [ -x test.sh ]; then
		echo "=== START TEST $i" >> "$RESULTS"
		echo "    [TEST/fuzz]   $name"
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
