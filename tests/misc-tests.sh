#!/bin/bash
#
# Misc tests

unset TOP
unset LANG
LANG=C
SCRIPT_DIR=$(dirname $(readlink -f $0))
TOP=$(readlink -f $SCRIPT_DIR/../)
TEST_DEV=${TEST_DEV:-}
RESULTS="$TOP/tests/misc-tests-results.txt"
IMAGE="$TOP/tests/test.img"

source $TOP/tests/common

# Allow child test to use $TOP and $RESULTS
export TOP
export RESULTS
# For custom script needs to verfiy recovery
export LANG
# For tests that only use a loop device
export IMAGE

rm -f $RESULTS

# test rely on corrupting blocks tool
check_prereq btrfs-corrupt-block
check_prereq btrfs-image
check_prereq btrfstune
check_prereq btrfs

# The tests are driven by their custom script called 'test.sh'

for i in $(find $TOP/tests/misc-tests -maxdepth 1 -mindepth 1 -type d	\
	${TEST:+-name "$TEST"} | sort)
do
	echo "    [TEST/misc]   $(basename $i)"
	cd $i
	echo "=== Entering $i" >> $RESULTS
	if [ -x test.sh ]; then
		./test.sh
		if [ $? -ne 0 ]; then
			_fail "test failed for case $(basename $i)"
		fi
	fi
	cd $TOP
done
