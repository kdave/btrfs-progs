#!/bin/bash
#
# loop through all of our bad images and make sure fsck repairs them properly

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
RESULTS="$TEST_TOP/fsck-tests-results.txt"
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

# test rely on corrupting blocks tool
check_prereq btrfs-corrupt-block
check_prereq btrfs-image
check_prereq btrfs
check_prereq btrfstune
check_kernel_support

run_one_test() {
	local testname

	testname="$1"
	echo "    [TEST/fsck]   $(basename $testname)"
	cd "$testname"
	echo "=== START TEST $testname" >> "$RESULTS"
	if [ -x test.sh ]; then
		# Type 2
		./test.sh
		if [ $? -ne 0 ]; then
			if [[ $TEST_LOG =~ dump ]]; then
				cat "$RESULTS"
			fi
			_fail "test failed for case $(basename $testname)"
		fi
	else
		# Type 1
		check_all_images
	fi
	cd "$TEST_TOP"
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

for i in $(find "$TEST_TOP/fsck-tests" -maxdepth 1 -mindepth 1 -type d	\
	${TEST:+-name "$TEST"} | sort)
do
	run_one_test "$i"
done
