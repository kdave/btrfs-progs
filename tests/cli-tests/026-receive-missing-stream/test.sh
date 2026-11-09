#!/bin/bash
# Receive must fail when the stream file cannot be opened, also with --dump

source "$TEST_TOP/common" || exit

tmp=$(_mktemp_dir receive)
stream="$tmp/missing.stream"

run_mustfail "receive succeeded with a missing stream file" \
	"$TOP/btrfs" receive -f "$stream" "$tmp"
run_mustfail "receive --dump succeeded with a missing stream file" \
	"$TOP/btrfs" receive --dump -f "$stream"

rmdir "$tmp"
