#!/bin/bash
#
# let dump-super dump random data, must not crash

source $TOP/tests/common

check_prereq btrfs

run_check $TOP/btrfs inspect-internal dump-super /dev/urandom
run_check $TOP/btrfs inspect-internal dump-super -a /dev/urandom
run_check $TOP/btrfs inspect-internal dump-super -fa /dev/urandom
run_check $TOP/btrfs inspect-internal dump-super -Ffa /dev/urandom
run_check $TOP/btrfs inspect-internal dump-super -Ffa /dev/urandom
run_check $TOP/btrfs inspect-internal dump-super -Ffa /dev/urandom
run_check $TOP/btrfs inspect-internal dump-super -Ffa /dev/urandom
run_check $TOP/btrfs inspect-internal dump-super -Ffa /dev/urandom
run_check $TOP/btrfs inspect-internal dump-super -Ffa /dev/urandom
