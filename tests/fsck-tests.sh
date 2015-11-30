#!/bin/bash
#
# loop through all of our bad images and make sure fsck repairs them properly
#
# It's GPL, same as everything else in this tree.
#

unset TOP
unset LANG
LANG=C
SCRIPT_DIR=$(dirname $(readlink -f $0))
TOP=$(readlink -f $SCRIPT_DIR/../)
TEST_DEV=${TEST_DEV:-}
RESULTS="$TOP/tests/fsck-tests-results.txt"

source $TOP/tests/common

# Allow child test to use $TOP and $RESULTS
export TOP
export RESULTS
# For custom script needs to verfiy recovery
export LANG

rm -f $RESULTS

# test rely on corrupting blocks tool
check_prereq btrfs-corrupt-block
check_prereq btrfs-image
check_prereq btrfs

run_one_test() {
	local testname

	testname="$1"
	echo "    [TEST/fsck]   $(basename $testname)"
	cd $testname
	echo "=== Entering $testname" >> $RESULTS
	if [ -x test.sh ]; then
		# Type 2
		./test.sh
		if [ $? -ne 0 ]; then
			_fail "test failed for case $(basename $testname)"
		fi
	else
		# Type 1
		check_all_images `pwd`
	fi
	cd $TOP
}

# Each dir contains one type of error for btrfsck test.
# Each dir must be one of the following 2 types:
# 1) Only btrfs-image dump
#    Only contains one or several btrfs-image dumps (.img)
#    Each image will be tested by generic test routine
#    (btrfsck --repair and btrfsck).
#    This is for case that btree-healthy images.
# 2) Custom test script
#    This dir contains test.sh which will do custom image
#    generation/check/verification.
#    This is for case btrfs-image can't dump or case needs extra
#    check/verify

for i in $(find $TOP/tests/fsck-tests -maxdepth 1 -mindepth 1 -type d	\
	${TEST:+-name "$TEST"} | sort)
do
	run_one_test "$i"
done
