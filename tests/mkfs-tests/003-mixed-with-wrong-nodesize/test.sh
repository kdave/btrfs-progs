#!/bin/bash
#
# Mixed mode needs equal sectorsize and nodesize

source $TOP/tests/common

check_prereq mkfs.btrfs

run_mayfail "$TOP/mkfs.btrfs" -b 512M -f -M -s 4096 -n 16384 "$TEST_DEV" && _fail

exit 0
