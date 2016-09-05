#!/bin/bash
# remove all intermediate files from tests

SCRIPT_DIR=$(dirname $(readlink -f $0))
TOP=$(readlink -f $SCRIPT_DIR/../)
source $TOP/tests/common

setup_root_helper

if [ "$BUILD_VERBOSE" = 1 ]; then
	verbose=-print
fi

$SUDO_HELPER umount "$TEST_MNT" &>/dev/null

if ! cd $TOP/tests; then
	echo "ERROR: cannot cd to $TOP/tests"
	exit 1
fi

find fsck-tests -type f -name '*.restored' $verbose -delete

# do not remove, the file could have special permissions set
echo -n > test.img
