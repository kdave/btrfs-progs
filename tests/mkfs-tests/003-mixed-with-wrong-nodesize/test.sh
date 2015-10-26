#!/bin/bash
#
# Mixed mode needs equal sectorsize and nodesize

source $TOP/tests/common

check_prereq mkfs.btrfs

run_check truncate -s 512M $IMAGE
run_mayfail $TOP/mkfs.btrfs -f -M -s 4096 -n 16384 "$IMAGE" && _fail

exit 0
