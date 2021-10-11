#!/bin/bash
# test for 'filesystem show' on fresh local file

source "$TEST_TOP/common"

check_prereq mkfs.btrfs
check_prereq btrfs

IMAGE=$(mktemp -u btrfs-progs-image.XXXXXX)

run_check truncate -s3g "$IMAGE"
run_check $SUDO_HELPER "$TOP/mkfs.btrfs" -f "$IMAGE"
run_check $SUDO_HELPER "$TOP/btrfs" filesystem show "$IMAGE"

rm -f "$IMAGE"
