#!/bin/bash
# Around 2014, btrfs kernel has a regression that create inline extent
# with ram_bytes offset by one.
# This old regression could be caught by tree-check code.
# This test case will check if btrfs check could detect and repair it.

source "$TEST_TOP/common"

check_prereq btrfs

check_all_images
