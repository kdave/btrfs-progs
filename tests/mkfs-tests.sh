#!/bin/bash
#
# mkfs.btrfs tests

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
RESULTS="$TEST_TOP/mkfs-tests-results.txt"
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

check_prereq mkfs.btrfs
check_prereq btrfs
check_kernel_support

# The tests are driven by their custom script called 'test.sh'

for i in $(find "$TEST_TOP/mkfs-tests" -maxdepth 1 -mindepth 1 -type d	\
	${TEST:+-name "$TEST"} | sort)
do
	echo "    [TEST/mkfs]   $(basename $i)"
	cd "$i"
	echo "=== START TEST $i" >> "$RESULTS"
	if [ -x test.sh ]; then
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
