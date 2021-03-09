#!/bin/bash
# a shell with test environment set up, logged commands and output

LANG=C
SCRIPT_DIR=$(dirname $(readlink -f "$0"))
TOP=$(readlink -f "$SCRIPT_DIR/../")
TEST_TOP="$TOP/tests"
INTERNAL_BIN="$TOP"
TEST_DEV=${TEST_DEV:-}
RESULTS="$TOP/tests/test-console.txt"
IMAGE="$TOP/tests/test.img"

source common
source common.convert

setup_root_helper

echo "Eval loop in test environment (log: $RESULTS)"
echo -e " ---------------------\nStarting session, `date`" >> "$RESULTS"
echo -n "`pwd`> "
while read x; do
	echo "COMMAND: $x" >> "$RESULTS"
	{ eval $x; } 2>&1 | tee -a "$RESULTS"
	echo -n "`pwd`> "
done
