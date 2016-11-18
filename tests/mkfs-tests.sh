#!/bin/bash
#
# mkfs.btrfs tests

LANG=C
SCRIPT_DIR=$(dirname $(readlink -f "$0"))
TOP=$(readlink -f "$SCRIPT_DIR/../")
TEST_DEV=${TEST_DEV:-}
RESULTS="$TOP/tests/mkfs-tests-results.txt"
IMAGE="$TOP/tests/test.img"

source "$TOP/tests/common"

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

for i in $(find "$TOP/tests/mkfs-tests" -maxdepth 1 -mindepth 1 -type d	\
	${TEST:+-name "$TEST"} | sort)
do
	echo "    [TEST/mkfs]   $(basename $i)"
	cd "$i"
	echo "=== Entering $i" >> "$RESULTS"
	if [ -x test.sh ]; then
		./test.sh
		if [ $? -ne 0 ]; then
			if [[ $TEST_LOG =~ dump ]]; then
				cat "$RESULTS"
			fi
			_fail "test failed for case $(basename $i)"
		fi
	fi
	cd "$TOP"
done
