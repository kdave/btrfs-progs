#!/bin/bash
# remove all intermediate files from tests

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
	echo "WARNING: cannot find btrfs in TOP=$TOP"
fi
TEST_DEV=${TEST_DEV:-}
RESULTS="$TEST_TOP/cli-tests-results.txt"
IMAGE="$TEST_TOP/test.img"

source "$TEST_TOP/common"

setup_root_helper

if [ "$BUILD_VERBOSE" = 1 ]; then
	verbose=-print
fi

$SUDO_HELPER umount -R "$TEST_MNT" &>/dev/null

if ! cd "$TEST_TOP"; then
	echo "ERROR: cannot cd to $TEST_TOP"
	exit 1
fi

find fsck-tests -type f -name '*.restored' $verbose -delete

# do not remove, the file could have special permissions set
echo -n > test.img
