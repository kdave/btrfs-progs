#!/bin/bash

source "$TEST_TOP/common"

check_prereq btrfs

image=$(extract_image "./default_case.raw.xz")
run_check_stdout "$TOP/btrfs" check "$image" 2>&1 |
	grep -q "crossing stripe boundary" ||
	_fail "no expected error message in the output"

rm -f "$image"
