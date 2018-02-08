#!/bin/bash
#
# Misc tests

LANG=C
SCRIPT_DIR=$(dirname $(readlink -f "$0"))
TEST_TOP=$(readlink -f "$SCRIPT_DIR/../tests/")
INTERNAL_BIN=$(readlink -f "$SCRIPT_DIR/../")
TOP=$(readlink -f "$SCRIPT_DIR/../")
if ! [ -f "$TOP/btrfs" ];then
	TOP=$(dirname `which btrfs`)
fi
TEST_DEV=${TEST_DEV:-}
RESULTS="$TEST_TOP/misc-tests-results.txt"
IMAGE="$TEST_TOP/test.img"

source "$TEST_TOP/common"

export INTERNAL_BIN
export TEST_TOP
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
check_prereq btrfs-zero-log
check_prereq btrfs-find-root
check_prereq btrfs-select-super
check_kernel_support

# The tests are driven by their custom script called 'test.sh'

for i in $(find "$TEST_TOP/misc-tests" -maxdepth 1 -mindepth 1 -type d	\
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
	cd "$TEST_TOP"
done
