#!/bin/bash
#
# confirm whether check detects name and hash mismatch in dir_item

source "$TEST_TOP/common"

check_prereq btrfs

image=$(extract_image "./default_case.img.xz")

run_mustfail "dir_item hash mismatch not found" "$TOP/btrfs" check "$image"

rm -f "$image"
