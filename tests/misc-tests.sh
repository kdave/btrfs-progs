#!/bin/bash
#
# Misc tests

LANG=C
SCRIPT_DIR=$(dirname $(readlink -f "$0"))
TOP=$(readlink -f "$SCRIPT_DIR/../")
TEST_DEV=${TEST_DEV:-}
RESULTS="$TOP/tests/misc-tests-results.txt"
IMAGE="$TOP/tests/test.img"

source "$TOP/tests/common"

export TOP
export RESULTS
export LANG
export TEST_DEV
export IMAGE

rm -f "$RESULTS"

# test rely on corrupting blocks tool
check_prereq btrfs-corrupt-block
check_prereq btrfs-image
check_prereq btrfstune
check_prereq btrfs
check_kernel_support

# The tests are driven by their custom script called 'test.sh'

for i in $(find "$TOP/tests/misc-tests" -maxdepth 1 -mindepth 1 -type d	\
	${TEST:+-name "$TEST"} | sort)
do
	echo "    [TEST/misc]   $(basename $i)"
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
