#!/bin/bash
# https://bugzilla.kernel.org/show_bug.cgi?id=200085
#
# The --dump option should follow the --max-errors and not loop indefinetelly
# by default

source "$TEST_TOP/common"

check_prereq btrfs

printf 'btrfs-stream\0\0\0\0\0' | run_mustfail "parsing invalid stream did not fail" \
	"$TOP/btrfs" receive --dump
printf 'btrfs-stream\0\0\0\0\0' | run_mustfail "parsing invalid stream did not fail" \
	"$TOP/btrfs" receive --dump -E 1
printf 'btrfs-stream\0\0\0\0\0' | run_mustfail "parsing invalid stream did not fail" \
	"$TOP/btrfs" receive --dump -E 10
