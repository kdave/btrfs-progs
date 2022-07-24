#!/bin/bash
#
# let dump-super dump random data, must not crash

source "$TEST_TOP/common"

check_prereq btrfs

run_mustfail "attempt to print bad superblock without force" \
	"$TOP/btrfs" inspect-internal dump-super /dev/urandom
run_mustfail "attempt to print bad superblock without force" \
	"$TOP/btrfs" inspect-internal dump-super -a /dev/urandom
run_mustfail "attempt to print bad superblock without force" \
	"$TOP/btrfs" inspect-internal dump-super -fa /dev/urandom

# All forced, no failure
run_check "$TOP/btrfs" inspect-internal dump-super -Ffa /dev/urandom
run_check "$TOP/btrfs" inspect-internal dump-super -Ffa /dev/urandom
run_check "$TOP/btrfs" inspect-internal dump-super -Ffa /dev/urandom
run_check "$TOP/btrfs" inspect-internal dump-super -Ffa /dev/urandom
run_check "$TOP/btrfs" inspect-internal dump-super -Ffa /dev/urandom
run_check "$TOP/btrfs" inspect-internal dump-super -Ffa /dev/urandom
