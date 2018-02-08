#!/bin/bash
#
# $ btrfs check img
# Checking filesystem on img
# UUID: 17f2bf15-f4c2-4ebc-b1f7-39b7af26257a
# checking extents
# bad extent [29376512, 29392896), type mismatch with chunk
# bad extent [29442048, 29458432), type mismatch with chunk
# bad extent [29589504, 29605888), type mismatch with chunk
# ...
#
# a buggy check leads to the above messages

source "$TEST_TOP/common"

check_prereq btrfs

image=$(extract_image "./default_case.img.xz")
run_check_stdout "$TOP/btrfs" check "$image" 2>&1 |
	grep -q "type mismatch with chunk" &&
	_fail "unexpected error message in the output"

rm -f "$image"
