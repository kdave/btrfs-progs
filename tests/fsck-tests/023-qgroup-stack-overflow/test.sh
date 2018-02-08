#!/bin/bash
#
# Check whether btrfs check quota verify will cause stack overflow.
# This is caused by lack of handling of tree reloc tree.
# Fixed by patch:
# btrfs-progs: Fix stack overflow for checking qgroup on tree reloc tree

source "$TEST_TOP/common"

check_prereq btrfs

check_image()
{
	run_check "$TOP/btrfs" check "$1"
}

check_all_images
